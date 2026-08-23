#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci=${LITE_SQLCI:-$repo_root/core/sql/lib/linux/64bit/debug/sqlci}
sql_lib_dir=${LITE_SQL_LIB_DIR:-$repo_root/core/sql/lib/linux/64bit/debug}
sqf_lib_dir=${LITE_SQF_LIB_DIR:-$repo_root/core/sqf/export/lib64d}
traf_home=$repo_root/core/sqf
run_root=$(mktemp -d /tmp/traf-lite-storage-cutover.XXXXXX)
store_root=$run_root/store
trap 'rm -rf "$run_root"' EXIT

run_sqlci() {
  local selected_store=$1
  shift
  env TRAF_HOME="$traf_home" TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$selected_store" \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$@" "$sqlci"
}

require_clean_sql() {
  local output=$1
  local operation=$2
  if grep -q '\*\*\* ERROR' <<<"$output"; then
    echo "$output" >&2
    echo "FAIL: $operation" >&2
    exit 1
  fi
}

# The old per-table layout is an incompatible format, not an implicit
# migration or fallback source.
mkdir -p "$run_root/old-layout/catalog"
set +e
old_layout_output=$(run_sqlci "$run_root/old-layout" 2>&1 <<'SQL'
SELECT 1 FROM (VALUES(1)) AS T(N);
exit;
SQL
)
old_layout_rc=$?
set -e
if [[ $old_layout_rc -eq 0 ]] && ! grep -q '\*\*\* ERROR' <<<"$old_layout_output"; then
  echo "$old_layout_output" >&2
  echo "FAIL: legacy per-table layout was accepted" >&2
  exit 1
fi
grep -q 'legacy per-table RocksDB layout is unsupported' \
  <<<"$old_layout_output" || {
  echo "$old_layout_output" >&2
  echo "FAIL: legacy layout rejection was not explicit" >&2
  exit 1
}
[[ ! -e "$run_root/old-layout/transactiondb" ]] || {
  echo "FAIL: rejected legacy layout created a TransactionDB" >&2
  exit 1
}

# A crash after writing the format marker must be retryable without another
# storage representation or a rollback source.
set +e
run_sqlci "$store_root" TRAF_LITE_ACTIVATION_FAULT=after-format \
  <<'SQL' >/dev/null 2>&1
SELECT 1 FROM (VALUES(1)) AS T(N);
exit;
SQL
fault_rc=$?
set -e
[[ $fault_rc -eq 91 ]] || {
  echo "FAIL: after-format activation fault returned $fault_rc, expected 91" >&2
  exit 1
}
[[ -f "$store_root/transactiondb/CURRENT" ]] || {
  echo "FAIL: format preparation did not create TransactionDB" >&2
  exit 1
}
[[ ! -e "$store_root/catalog" && ! -e "$store_root/data" ]] || {
  echo "FAIL: format preparation created a legacy storage directory" >&2
  exit 1
}

active_output=$(run_sqlci "$store_root" <<'SQL'
CREATE TABLE M13_UNIFIED (
  id INT NOT NULL,
  name VARCHAR(32) UNIQUE,
  PRIMARY KEY(id)
);
CREATE INDEX M13_UNIFIED_I ON M13_UNIFIED(name);
INSERT INTO M13_UNIFIED VALUES (1, 'one'), (2, 'two');
UPDATE M13_UNIFIED SET name = 'two-new' WHERE id = 2;
SELECT id, name FROM M13_UNIFIED ORDER BY id;
exit;
SQL
)
require_clean_sql "$active_output" "activate and use unified storage"
grep -q -- '--- 2 row(s) selected.' <<<"$active_output" || {
  echo "$active_output" >&2
  echo "FAIL: unified storage row count" >&2
  exit 1
}
grep -q 'two-new' <<<"$active_output" || {
  echo "$active_output" >&2
  echo "FAIL: unified storage update was not visible" >&2
  exit 1
}

restart_output=$(run_sqlci "$store_root" <<'SQL'
SELECT id, name FROM M13_UNIFIED ORDER BY id;
DROP INDEX M13_UNIFIED_I;
DROP TABLE M13_UNIFIED;
CREATE TABLE M13_NEW (
  id INT NOT NULL PRIMARY KEY,
  value_col VARCHAR(32)
);
INSERT INTO M13_NEW VALUES (10, 'unified-only');
SELECT id, value_col FROM M13_NEW;
exit;
SQL
)
require_clean_sql "$restart_output" "restart and run unified-only DDL/DML"
grep -q 'unified-only' <<<"$restart_output" || {
  echo "$restart_output" >&2
  echo "FAIL: unified-only restart result" >&2
  exit 1
}
[[ ! -e "$store_root/catalog" && ! -e "$store_root/data" ]] || {
  echo "FAIL: runtime recreated a legacy storage directory" >&2
  exit 1
}

echo "Lite exclusive unified storage checks passed"
