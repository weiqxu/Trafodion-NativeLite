#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci="$repo_root/core/sql/lib/linux/64bit/debug/sqlci"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"
output=

fail() {
  echo "FAIL: $*" >&2
  if [[ -n "$output" ]]; then
    echo "----- SQLCI output -----" >&2
    printf '%s\n' "$output" >&2
  fi
  exit 1
}

[[ -x "$sqlci" ]] || fail "missing built lite sqlci: $sqlci"

store_dir=$(mktemp -d /tmp/traf-lite-context-transactions.XXXXXX)
trap 'rm -rf "$store_dir"' EXIT

run_sqlci() {
  env LC_ALL=C \
    SQLCI_CURSOR=1 \
    TRAF_HOME="$traf_home" \
    TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$sqlci"
}

# A fresh SQLCI process owns the default handle 2000. CREATECONTEXT allocates
# 2001 and 2002 deterministically from CliGlobals::nextUniqueContextHandle.
# Both non-default ContextCli sessions keep active transactions while SQLCI
# switches between them, so this exercises the production CLI/compiler/
# executor path rather than only the Lite Storage transaction facade.
output=$(run_sqlci 2>&1 <<'SQL'
CREATE TABLE ctx_t(k INT NOT NULL PRIMARY KEY, v VARCHAR(24));
CREATECONTEXT;
CREATECONTEXT;
SWITCHCONTEXT 2001;
BEGIN WORK;
INSERT INTO ctx_t VALUES (1, 'a-pending');
SWITCHCONTEXT 2002;
SELECT COUNT(*) AS ISOCOUNT FROM ctx_t WHERE k = 1;
BEGIN WORK;
INSERT INTO ctx_t VALUES (1, 'b-pending');
SWITCHCONTEXT 2001;
COMMIT WORK;
SWITCHCONTEXT 2002;
COMMIT WORK;
SELECT COUNT(*) AS AFTERCON FROM ctx_t;
BEGIN WORK;
INSERT INTO ctx_t VALUES (2, 'b-commit');
SWITCHCONTEXT 2001;
BEGIN WORK;
INSERT INTO ctx_t VALUES (3, 'a-rollback');
SWITCHCONTEXT 2002;
COMMIT WORK;
SWITCHCONTEXT 2001;
ROLLBACK WORK;
SELECT COUNT(*) AS INDEPCNT FROM ctx_t;
BEGIN WORK;
INSERT INTO ctx_t VALUES (4, 'discard-reset');
SWITCHCONTEXT 2002;
RESETCONTEXT 2001;
SWITCHCONTEXT 2001;
SELECT COUNT(*) AS RESETCNT FROM ctx_t WHERE k = 4;
BEGIN WORK;
INSERT INTO ctx_t VALUES (5, 'after-reset');
COMMIT WORK;
SELECT COUNT(*) AS AFTERRST FROM ctx_t;
BEGIN WORK;
INSERT INTO ctx_t VALUES (6, 'discard-delete');
SWITCHCONTEXT 2002;
DELETECONTEXT 2001;
SELECT COUNT(*) AS DELETECNT FROM ctx_t WHERE k = 6;
BEGIN WORK;
INSERT INTO ctx_t VALUES (6, 'after-delete');
COMMIT WORK;
SELECT COUNT(*) AS FINALCNT FROM ctx_t;
SELECT k, v FROM ctx_t ORDER BY k;
EXIT;
SQL
)

scalar_value() {
  local heading=$1
  awk -v heading="$heading" '
    index($0, heading) { found = 1; next }
    found && $0 ~ /^[[:space:]]*-+[[:space:]]*$/ { next }
    found && $0 ~ /^[[:space:]]*[0-9]+[[:space:]]*$/ {
      gsub(/[[:space:]]/, "", $0)
      print $0
      exit
    }
  ' <<<"$output"
}

grep -q 'success -- new handle:2001' <<<"$output" ||
  fail "first independent ContextCli was not created"
grep -q 'success -- new handle:2002' <<<"$output" ||
  fail "second independent ContextCli was not created"
[[ $(scalar_value ISOCOUNT) == 0 ]] ||
  fail "one CLI context observed the other context's uncommitted write"
[[ $(scalar_value AFTERCON) == 1 ]] ||
  fail "same-key conflict did not leave exactly the first committed row"
[[ $(scalar_value INDEPCNT) == 2 ]] ||
  fail "independent commit/rollback produced an unexpected row count"
[[ $(scalar_value RESETCNT) == 0 ]] ||
  fail "RESETCONTEXT published its pending write"
[[ $(scalar_value AFTERRST) == 3 ]] ||
  fail "reset ContextCli was not reusable"
[[ $(scalar_value DELETECNT) == 0 ]] ||
  fail "DELETECONTEXT published its pending write"
[[ $(scalar_value FINALCNT) == 4 ]] ||
  fail "surviving ContextCli was poisoned by peer deletion"

error_count=$(grep -c '\*\*\* ERROR' <<<"$output" || true)
[[ $error_count -eq 2 ]] ||
  fail "expected only the two-line same-key commit diagnostic"
grep -q 'ERROR\[8001\]' <<<"$output" ||
  fail "same-key commit did not return the executor failure"
grep -q 'SQLSTATE 40001, OCC read/write conflict, type=point' <<<"$output" ||
  fail "same-key commit did not return the deterministic point-conflict diagnostic"
[[ $(grep -c 'SQL operation failed with errors' <<<"$output" || true) -eq 1 ]] ||
  fail "an unexpected SQL operation failed"

for committed_value in a-pending b-commit after-reset after-delete; do
  grep -q "$committed_value" <<<"$output" ||
    fail "missing committed value: $committed_value"
done
if grep -qE 'a-rollback|discard-reset|discard-delete' <<<"$(
    sed -n '/^K[[:space:]]\+V[[:space:]]*$/,/row(s) selected/p' <<<"$output"
  )"; then
  fail "rolled-back/reset/deleted context value reached the final result"
fi

echo "lite ContextCli transaction probe passed"
