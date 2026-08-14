#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci=${LOCAL_LITE_SQLCI:-$repo_root/core/sql/lib/linux/64bit/debug/sqlci}
sql_lib_dir=${LOCAL_LITE_SQL_LIB_DIR:-$repo_root/core/sql/lib/linux/64bit/debug}
sqf_lib_dir=${LOCAL_LITE_SQF_LIB_DIR:-$repo_root/core/sqf/export/lib64d}
traf_home=$repo_root/core/sqf
store_dir=$(mktemp -d /tmp/traf-local-lite-sql-recovery.XXXXXX)
trap 'rm -rf "$store_dir"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

run_sqlci() {
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$sqlci"
}

[[ -x "$sqlci" ]] || fail "missing built sqlci: $sqlci"

setup_output=$(run_sqlci <<'SQL'
CREATE TABLE M12_CRASH_A (id INT, value VARCHAR(32));
CREATE TABLE M12_CRASH_B (
  id INT NOT NULL,
  value VARCHAR(32),
  PRIMARY KEY(id)
);
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$setup_output"; then
  echo "$setup_output" >&2
  fail "create SQL crash-recovery fixture"
fi

set +e
fault_output=$(
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_STORE_DIR="$store_dir" \
    TRAF_LOCAL_LITE_LEGACY_COMMIT_FAULT_AFTER_TABLE=1 \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$sqlci" <<'SQL'
BEGIN WORK;
INSERT INTO M12_CRASH_A VALUES (1, 'journal-a');
INSERT INTO M12_CRASH_B VALUES (2, 'journal-b');
COMMIT WORK;
exit;
SQL
)
fault_rc=$?
set -e
[[ $fault_rc -eq 90 ]] || {
  echo "$fault_output" >&2
  fail "commit fault exited with $fault_rc instead of 90"
}

# Opening the next SQL process must replay the durable journal before it can
# observe either table. The keyless A table was already published when the
# process stopped, while B still needs replay.
recovery_output=$(run_sqlci <<'SQL'
SELECT id, value FROM M12_CRASH_A ORDER BY id;
SELECT id, value FROM M12_CRASH_B ORDER BY id;
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$recovery_output" ||
   ! grep -q 'journal-a' <<<"$recovery_output" ||
   ! grep -q 'journal-b' <<<"$recovery_output" ||
   [[ $(grep -c -- '--- 1 row(s) selected.' <<<"$recovery_output") -ne 2 ]]; then
  echo "$recovery_output" >&2
  fail "startup did not recover the complete SQL transaction"
fi

# A second restart proves replay is idempotent and the completed journal was
# removed rather than duplicating either base row or its indexes.
idempotent_output=$(run_sqlci <<'SQL'
SELECT COUNT(*) FROM M12_CRASH_A;
SELECT COUNT(*) FROM M12_CRASH_B;
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$idempotent_output" ||
   [[ $(grep -Ec '^ *1 *$' <<<"$idempotent_output") -ne 2 ]]; then
  echo "$idempotent_output" >&2
  fail "recovered SQL transaction was not idempotent"
fi

echo "LocalLite SQL multi-table commit recovery checks passed"
