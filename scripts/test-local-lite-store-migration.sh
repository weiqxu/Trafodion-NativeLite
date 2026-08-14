#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci=${LOCAL_LITE_SQLCI:-$repo_root/core/sql/lib/linux/64bit/debug/sqlci}
sql_lib_dir=${LOCAL_LITE_SQL_LIB_DIR:-$repo_root/core/sql/lib/linux/64bit/debug}
sqf_lib_dir=${LOCAL_LITE_SQF_LIB_DIR:-$repo_root/core/sqf/export/lib64d}
traf_home=$repo_root/core/sqf
cxx=${CXX:-/usr/bin/g++}
run_root=$(mktemp -d /tmp/traf-local-lite-store-migration.XXXXXX)
trap 'rm -rf "$run_root"' EXIT

"$cxx" -std=c++11 -Wall -Wextra -Werror -pthread \
  -I"${ROCKSDB_INC_DIR:-/usr/include}" \
  -I"$repo_root/core/sql/localstore" \
  "$repo_root/core/sql/localstore/LocalLiteStorage.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteStorageMigration.cpp" \
  "$repo_root/scripts/local-lite-store-migration-test.cpp" \
  -L"${ROCKSDB_LIB_DIR:-/usr/lib/x86_64-linux-gnu}" \
  -lrocksdb -lcrypto -o "$run_root/store-migration-test"

run_sqlci() {
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_STORE_DIR="$run_root/legacy" \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$sqlci"
}

setup_output=$(run_sqlci <<'SQL'
CREATE TABLE M12_OLD_LAYOUT (
  id INT NOT NULL,
  name VARCHAR(32) UNIQUE,
  PRIMARY KEY(id)
);
CREATE INDEX M12_OLD_LAYOUT_I ON M12_OLD_LAYOUT(name);
INSERT INTO M12_OLD_LAYOUT VALUES (1, 'one'), (2, 'two');
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$setup_output"; then
  echo "$setup_output" >&2
  echo "FAIL: create old-layout migration fixture" >&2
  exit 1
fi

set +e
TRAF_LOCAL_LITE_MIGRATION_FAULT_AFTER_RECORDS=2 \
  "$run_root/store-migration-test" migrate "$run_root/legacy" \
  "$run_root/interrupted-target" >/dev/null 2>&1
fault_rc=$?
set -e
[[ $fault_rc -ne 0 ]] || {
  echo "FAIL: injected old-layout migration did not fail" >&2
  exit 1
}

# The failed copy was one target transaction. A retry against the same target
# must succeed without cleanup, proving no partial format marker or data leaked.
"$run_root/store-migration-test" migrate "$run_root/legacy" \
  "$run_root/interrupted-target"
"$run_root/store-migration-test" verify "$run_root/legacy" \
  "$run_root/interrupted-target"

"$run_root/store-migration-test" migrate "$run_root/legacy" \
  "$run_root/migrated"
"$run_root/store-migration-test" verify "$run_root/legacy" \
  "$run_root/migrated"

# The legacy source is the rollback window and remains queryable after copy.
source_output=$(run_sqlci <<'SQL'
SELECT id, name FROM M12_OLD_LAYOUT ORDER BY id;
exit;
SQL
)
if grep -q '\*\*\* ERROR' <<<"$source_output" ||
   ! grep -q -- '--- 2 row(s) selected.' <<<"$source_output"; then
  echo "$source_output" >&2
  echo "FAIL: legacy rollback source changed during migration" >&2
  exit 1
fi
echo "LocalLite old-layout migration checks passed"
