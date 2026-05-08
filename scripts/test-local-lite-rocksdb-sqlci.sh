#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci="$repo_root/core/sql/lib/linux/64bit/debug/sqlci"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$sqlci" ]] || fail "missing built sqlci: $sqlci"

store_dir=$(mktemp -d /tmp/traf-local-lite-rocksdb-sqlci.XXXXXX)
trap 'rm -rf "$store_dir"' EXIT

run_sqlci() {
  env TRAF_HOME="$traf_home" \
    TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$sqlci"
}

ldd_output=$(
  env LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" ldd "$sqlci"
)

grep -q 'librocksdb' <<<"$ldd_output" ||
  fail "sqlci must link to librocksdb in local-lite"
if grep -Eq 'lib(jvm|hdfs|hadoop|zookeeper|hbase)' <<<"$ldd_output"; then
  fail "sqlci must not link Java/Hadoop/HBase/ZooKeeper libraries"
fi

create_output=$(
  printf "CREATE TABLE t(a INT, b VARCHAR(20));\nINSERT INTO t VALUES (1, 'one'), (2, 'two');\nexit;\n" |
    run_sqlci
)
grep -q -- '--- SQL operation complete.' <<<"$create_output" ||
  fail "CREATE TABLE did not complete"
grep -q -- '--- 2 row(s) inserted.' <<<"$create_output" ||
  fail "multi-row INSERT did not insert two rows"

duplicate_output=$(
  printf "CREATE TABLE t(a INT, b VARCHAR(20));\nexit;\n" |
    run_sqlci
)
grep -q 'local-lite table already exists' <<<"$duplicate_output" ||
  fail "duplicate CREATE TABLE diagnostic missing"

bind_output=$(
  printf "SELECT a FROM t;\nexit;\n" |
    run_sqlci
)
grep -q 'HDFS, Hive, and HBase are not supported in local-lite builds' <<<"$bind_output" ||
  fail "local catalog NATable bind did not reach local-lite table scan stub"
if grep -q 'does not exist or is inaccessible' <<<"$bind_output"; then
  fail "local catalog NATable bind reported table missing"
fi

error_output=$(
  printf "SELECT * FROM missing_table;\nINSERT INTO t VALUES (1);\nCREATE TABLE bad_lob(c BLOB(100));\nexit;\n" |
    run_sqlci
)
grep -q 'local-lite table does not exist' <<<"$error_output" ||
  fail "missing table diagnostic missing"
grep -q 'INSERT value count does not match local-lite table column count' <<<"$error_output" ||
  fail "INSERT column count diagnostic missing"
grep -q 'unsupported local-lite column type' <<<"$error_output" ||
  fail "unsupported column type diagnostic missing"

select_output=$(
  printf "SELECT * FROM t;\nDROP TABLE t;\nexit;\n" |
    run_sqlci
)
grep -q '1 one' <<<"$select_output" ||
  fail "SELECT did not return persisted row after reopening sqlci"
grep -q '2 two' <<<"$select_output" ||
  fail "SELECT did not return second persisted row after reopening sqlci"
grep -q -- '--- 2 row(s) selected.' <<<"$select_output" ||
  fail "SELECT did not report two selected rows"

unsupported_output=$(
  printf "UPDATE t SET a = 2;\nDELETE FROM t;\nCREATE INDEX ix ON t(a);\nUPSERT INTO t VALUES (3, 'three');\nCREATE VIEW v AS SELECT * FROM t;\nALTER TABLE t ADD COLUMN c INT;\nTRUNCATE TABLE t;\nCREATE TABLE constrained(a INT PRIMARY KEY);\nexit;\n" |
    run_sqlci
)
grep -q 'UPDATE, DELETE, and MERGE are not supported in local-lite' <<<"$unsupported_output" ||
  fail "UPDATE/DELETE unsupported diagnostic missing"
grep -q 'CREATE INDEX is not supported in local-lite' <<<"$unsupported_output" ||
  fail "CREATE INDEX unsupported diagnostic missing"
grep -q 'UPSERT is not supported in local-lite v1; use INSERT' <<<"$unsupported_output" ||
  fail "UPSERT unsupported diagnostic missing"
grep -q 'CREATE VIEW is not supported in local-lite' <<<"$unsupported_output" ||
  fail "CREATE VIEW unsupported diagnostic missing"
grep -q 'ALTER TABLE is not supported in local-lite' <<<"$unsupported_output" ||
  fail "ALTER TABLE unsupported diagnostic missing"
grep -q 'TRUNCATE TABLE is not supported in local-lite' <<<"$unsupported_output" ||
  fail "TRUNCATE TABLE unsupported diagnostic missing"
grep -q 'local-lite table constraints are not supported' <<<"$unsupported_output" ||
  fail "constraint unsupported diagnostic missing"

echo "local-lite RocksDB sqlci smoke passed"
