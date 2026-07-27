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
grep -Eq '^ *1 *$' <<<"$bind_output" ||
  fail "executor scan did not return projected local-lite row"
grep -Eq '^ *2 *$' <<<"$bind_output" ||
  fail "executor scan did not return second projected local-lite row"
grep -q -- '--- 2 row(s) selected.' <<<"$bind_output" ||
  fail "executor scan did not report two selected rows"
if grep -q 'does not exist or is inaccessible' <<<"$bind_output"; then
  fail "local catalog NATable bind reported table missing"
fi

projection_output=$(
  printf "SELECT b FROM t;\nSELECT a, b FROM t;\nSELECT b FROM t WHERE a = 1;\nexit;\n" |
    run_sqlci
)
grep -q 'one' <<<"$projection_output" ||
  fail "executor scan did not return projected VARCHAR row"
grep -q 'two' <<<"$projection_output" ||
  fail "executor scan did not return second projected VARCHAR row"
grep -q -- '--- 2 row(s) selected.' <<<"$projection_output" ||
  fail "executor scan projection did not report selected rows"
grep -q -- '--- 1 row(s) selected.' <<<"$projection_output" ||
  fail "executor scan predicate did not filter projected rows"

self_join_output=$(
  printf "SELECT t1.b, t2.b FROM t t1, t t2 WHERE t1.a = t2.a AND t1.a = 1;\nexit;\n" |
    run_sqlci
)
grep -Eq '^one[[:space:]]+one[[:space:]]*$' <<<"$self_join_output" ||
  fail "self-join over local-lite table did not return expected row"
grep -q -- '--- 1 row(s) selected.' <<<"$self_join_output" ||
  fail "self-join over local-lite table did not report one selected row"
if grep -q 'LOCK' <<<"$self_join_output"; then
  fail "self-join over local-lite table hit RocksDB LOCK"
fi

error_output=$(
  printf "INSERT INTO missing_table VALUES (1);\nINSERT INTO t VALUES (1);\nCREATE TABLE bad_lob(c BLOB(100));\nexit;\n" |
    run_sqlci
)
grep -q 'ERROR\[4082\]' <<<"$error_output" ||
  fail "missing table diagnostic missing"
grep -q 'ERROR\[4023\]' <<<"$error_output" ||
  fail "INSERT column count diagnostic missing"
grep -q 'unsupported local-lite column type' <<<"$error_output" ||
  fail "unsupported column type diagnostic missing"

select_output=$(
  printf "SELECT * FROM t;\nDROP TABLE t;\nexit;\n" |
    run_sqlci
)
grep -q '1  one' <<<"$select_output" ||
  fail "SELECT did not return persisted row after reopening sqlci"
grep -q '2  two' <<<"$select_output" ||
  fail "SELECT did not return second persisted row after reopening sqlci"
grep -q -- '--- 2 row(s) selected.' <<<"$select_output" ||
  fail "SELECT did not report two selected rows"

datetime_output=$(
  printf "CREATE TABLE dt(d DATE, tm TIME, ts TIMESTAMP, label VARCHAR(20));\nINSERT INTO dt VALUES (DATE '2026-07-03', TIME '12:34:56', TIMESTAMP '2026-07-03 12:34:56', 'hit'), (DATE '2026-07-04', TIME '01:02:03', TIMESTAMP '2026-07-04 01:02:03', 'miss');\nSELECT label FROM dt WHERE d = DATE '2026-07-03';\nSELECT label FROM dt WHERE tm = TIME '12:34:56';\nSELECT label FROM dt WHERE ts = TIMESTAMP '2026-07-03 12:34:56';\nDROP TABLE dt;\nexit;\n" |
    run_sqlci
)
grep -q 'hit' <<<"$datetime_output" ||
  fail "datetime predicate scan did not return matching row"
if grep -q 'miss' <<<"$datetime_output"; then
  fail "datetime predicate scan returned non-matching row"
fi
datetime_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$datetime_output")
[[ "$datetime_selected_count" -ge 3 ]] ||
  fail "datetime predicate scans did not each report one selected row"

numeric_output=$(
  printf "CREATE TABLE numtab(n NUMERIC(5,2), label VARCHAR(20));\nINSERT INTO numtab VALUES (12.34, 'ok'), (56.78, 'miss');\nSELECT label FROM numtab WHERE n = 12.34;\nDROP TABLE numtab;\nexit;\n" |
    run_sqlci
)
grep -q 'ok' <<<"$numeric_output" ||
  fail "numeric predicate scan did not return matching row"
if grep -q 'miss' <<<"$numeric_output"; then
  fail "numeric predicate scan returned non-matching row"
fi
grep -q -- '--- 1 row(s) selected.' <<<"$numeric_output" ||
  fail "numeric predicate scan did not report one selected row"

decimal_big_numeric_output=$(
  printf "CREATE TABLE dectab(d DECIMAL(5,2), label VARCHAR(20));\nINSERT INTO dectab VALUES (12.34, 'dec-hit'), (56.78, 'dec-miss');\nSELECT label FROM dectab WHERE d = 12.34;\nDROP TABLE dectab;\nCREATE TABLE bigtab(n NUMERIC(30,2), label VARCHAR(20));\nINSERT INTO bigtab VALUES (1234567890123456789012345678.90, 'big-hit'), (2234567890123456789012345678.90, 'big-miss');\nSELECT label FROM bigtab WHERE n = 1234567890123456789012345678.90;\nDROP TABLE bigtab;\nexit;\n" |
    run_sqlci
)
grep -q 'dec-hit' <<<"$decimal_big_numeric_output" ||
  fail "decimal predicate scan did not return matching row"
if grep -q 'dec-miss' <<<"$decimal_big_numeric_output"; then
  fail "decimal predicate scan returned non-matching row"
fi
grep -q 'big-hit' <<<"$decimal_big_numeric_output" ||
  fail "BigNum numeric predicate scan did not return matching row"
if grep -q 'big-miss' <<<"$decimal_big_numeric_output"; then
  fail "BigNum numeric predicate scan returned non-matching row"
fi
decimal_big_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$decimal_big_numeric_output")
[[ "$decimal_big_selected_count" -ge 2 ]] ||
  fail "decimal/BigNum predicate scans did not each report one selected row"

null_expr_output=$(
  printf "CREATE TABLE nul(a INT, b VARCHAR(20), c INT NOT NULL);\nINSERT INTO nul VALUES (NULL, 'null-a', 1), (2, NULL, 1), (1 + 2, CAST('expr' AS VARCHAR(20)), 1);\nSELECT b FROM nul WHERE a IS NULL;\nSELECT a FROM nul WHERE b IS NULL;\nSELECT b FROM nul WHERE a = 3;\nINSERT INTO nul VALUES (4, 'bad', NULL);\nDROP TABLE nul;\nCREATE TABLE vv(a VARCHAR(10), b VARCHAR(10), label VARCHAR(10));\nINSERT INTO vv VALUES (NULL, 'second', 'hit'), ('first', NULL, 'miss');\nSELECT b, label FROM vv WHERE a IS NULL;\nSELECT a, label FROM vv WHERE b IS NULL;\nDROP TABLE vv;\nexit;\n" |
    run_sqlci
)
grep -q 'null-a' <<<"$null_expr_output" ||
  fail "NULL numeric predicate scan did not return matching VARCHAR row"
grep -Eq '^ *2 *$' <<<"$null_expr_output" ||
  fail "NULL VARCHAR predicate scan did not return matching INT row"
grep -q 'expr' <<<"$null_expr_output" ||
  fail "executor expression INSERT did not persist expression result"
grep -q 'ERROR\[4122\]' <<<"$null_expr_output" ||
  fail "NOT NULL executor insert diagnostic missing"
grep -q 'second      hit' <<<"$null_expr_output" ||
  fail "multiple VARCHAR projection after NULL did not preserve following value"
grep -q 'first       miss' <<<"$null_expr_output" ||
  fail "multiple VARCHAR projection with later NULL did not preserve earlier value"
null_expr_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$null_expr_output")
[[ "$null_expr_selected_count" -ge 5 ]] ||
  fail "NULL/expression scans did not each report one selected row"

transaction_output=$(
  printf "CREATE TABLE tx(a INT, b VARCHAR(20));\nBEGIN WORK;\nINSERT INTO tx VALUES (1, 'rollback');\nSELECT b FROM tx WHERE a = 1;\nROLLBACK WORK;\nSELECT b FROM tx WHERE a = 1;\nBEGIN WORK;\nINSERT INTO tx VALUES (2, 'commit');\nCOMMIT WORK;\nSELECT b FROM tx WHERE a = 2;\nDROP TABLE tx;\nexit;\n" |
    run_sqlci
)
grep -q 'rollback' <<<"$transaction_output" ||
  fail "local transaction did not read its own pending insert"
grep -q 'commit' <<<"$transaction_output" ||
  fail "local transaction commit did not persist inserted row"
grep -q -- '--- 0 row(s) selected.' <<<"$transaction_output" ||
  fail "local transaction rollback did not discard inserted row"
transaction_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$transaction_output")
[[ "$transaction_selected_count" -ge 2 ]] ||
  fail "local transaction scans did not report expected selected rows"

primary_key_output=$(
  printf "CREATE TABLE pk_t(a INT PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_t VALUES (1, 'one');\nSELECT b FROM pk_t WHERE a = 1;\nINSERT INTO pk_t VALUES (1, 'dup');\nBEGIN WORK;\nINSERT INTO pk_t VALUES (2, 'two');\nSELECT b FROM pk_t WHERE a = 2;\nINSERT INTO pk_t VALUES (2, 'dup-pending');\nROLLBACK WORK;\nSELECT b FROM pk_t WHERE a = 2;\nDROP TABLE pk_t;\nexit;\n" |
    run_sqlci
)
grep -q 'one' <<<"$primary_key_output" ||
  fail "primary key table did not return inserted row"
grep -q 'two' <<<"$primary_key_output" ||
  fail "primary key transaction did not read pending row"
pk_duplicate_count=$(grep -c 'duplicate local-lite primary key' <<<"$primary_key_output")
[[ "$pk_duplicate_count" -ge 2 ]] ||
  fail "primary key duplicate diagnostics missing"
grep -q -- '--- 0 row(s) selected.' <<<"$primary_key_output" ||
  fail "primary key rollback did not discard pending row"

unsupported_output=$(
  printf "UPDATE t SET a = 2;\nDELETE FROM t;\nCREATE INDEX ix ON t(a);\nUPSERT INTO t VALUES (3, 'three');\nCREATE VIEW v AS SELECT * FROM t;\nALTER TABLE t ADD COLUMN c INT;\nTRUNCATE TABLE t;\nCREATE TABLE constrained(a INT CHECK (a > 0));\nexit;\n" |
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
grep -q 'local-lite table constraints other than PRIMARY KEY are not supported' <<<"$unsupported_output" ||
  fail "constraint unsupported diagnostic missing"

echo "local-lite RocksDB sqlci smoke passed"
