#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci=${LOCAL_LITE_SQLCI:-$repo_root/core/sql/lib/linux/64bit/debug/sqlci}
sql_lib_dir=${LOCAL_LITE_SQL_LIB_DIR:-$repo_root/core/sql/lib/linux/64bit/debug}
sqf_lib_dir=${LOCAL_LITE_SQF_LIB_DIR:-$repo_root/core/sqf/export/lib64d}
traf_home=$repo_root/core/sqf
cxx=${CXX:-/usr/bin/g++}
run_root=$(mktemp -d /tmp/traf-local-lite-metadata-migration.XXXXXX)
trap 'rm -rf "$run_root"' EXIT

"$cxx" -std=c++11 -Wall -Wextra -Werror \
  -I"${ROCKSDB_INC_DIR:-/usr/include}" \
  "$repo_root/scripts/local-lite-metadata-key-migration-test.cpp" \
  -L"${ROCKSDB_LIB_DIR:-/usr/lib/x86_64-linux-gnu}" -lrocksdb \
  -o "$run_root/metadata-key-migration-test"

run_sqlci() {
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_STORE_DIR="$run_root/store" \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$sqlci"
}

setup_output=$(run_sqlci <<'SQL'
CREATE TABLE M12_MIGRATION (
  id INT NOT NULL,
  name VARCHAR(32) UNIQUE,
  PRIMARY KEY(id)
);
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$setup_output"; then
  echo "$setup_output" >&2
  echo "FAIL: create metadata migration fixture" >&2
  exit 1
fi

"$run_root/metadata-key-migration-test" downgrade \
  "$run_root/store/transactiondb"

migration_output=$(run_sqlci <<'SQL'
SELECT COLUMN_NAME FROM TRAFODION."_MD_".COLUMNS;
SELECT COLUMN_NAME, KEYSEQ_NUMBER FROM TRAFODION."_MD_".KEYS;
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$migration_output"; then
  echo "$migration_output" >&2
  echo "FAIL: query metadata after key migration" >&2
  exit 1
fi
grep -q -- '--- 2 row(s) selected.' <<<"$migration_output" || {
  echo "$migration_output" >&2
  echo "FAIL: migrated metadata did not restore both columns and keys" >&2
  exit 1
}

"$run_root/metadata-key-migration-test" verify \
  "$run_root/store/transactiondb"
echo "LocalLite metadata-key migration checks passed"
