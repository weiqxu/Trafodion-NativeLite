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

run_sqlci_trace_scan() {
  env TRAF_HOME="$traf_home" \
    TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_LITE_TRACE_SCAN=1 \
    TRAF_LOCAL_LITE_TRACE_SNAPSHOT=1 \
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
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^one[[:space:]]+one[[:space:]]*$' <<<"$self_join_output" ||
  fail "self-join over local-lite table did not return expected row"
grep -q -- '--- 1 row(s) selected.' <<<"$self_join_output" ||
  fail "self-join over local-lite table did not report one selected row"
if grep -q 'LOCK' <<<"$self_join_output"; then
  fail "self-join over local-lite table hit RocksDB LOCK"
fi
self_join_snapshot_acquires=$(
  grep -c 'LOCAL_LITE_SNAPSHOT_ACQUIRE' <<<"$self_join_output"
)
[[ "$self_join_snapshot_acquires" -eq 1 ]] ||
  fail "self-join must acquire one statement snapshot for its shared table"
grep -q 'LOCAL_LITE_SNAPSHOT_REUSE' <<<"$self_join_output" ||
  fail "self-join scan TCBs did not reuse the statement snapshot"
grep -q 'LOCAL_LITE_SNAPSHOT_RELEASE' <<<"$self_join_output" ||
  fail "self-join statement snapshot was not released at executor completion"

query_shape_output=$(
  printf "CREATE TABLE shape_t(a INT, b VARCHAR(20), c INT);\nINSERT INTO shape_t VALUES (1, 'one', 10), (2, 'two', 20), (3, 'two', 30), (4, 'four', 20);\nSELECT a, b FROM shape_t ORDER BY c DESC, a ASC;\nSELECT l.b, r.a FROM shape_t l JOIN shape_t r ON l.c = r.c WHERE l.a = 2 ORDER BY r.a;\nSELECT l.a, r.b FROM shape_t l LEFT JOIN shape_t r ON l.a = r.a AND r.b = 'missing' WHERE l.a = 1;\nDROP TABLE shape_t;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^ *3[[:space:]]+two[[:space:]]*$' <<<"$query_shape_output" ||
  fail "ORDER BY query did not return highest c row first"
grep -Eq '^two[[:space:]]+4[[:space:]]*$' <<<"$query_shape_output" ||
  fail "inner join query did not return same-c matching row"
grep -q '          1  ?' <<<"$query_shape_output" ||
  fail "left join query did not preserve unmatched left row"
query_shape_scan_count=$(grep -c 'LOCAL_LITE_SCAN_FULL' <<<"$query_shape_output")
[[ "$query_shape_scan_count" -ge 5 ]] ||
  fail "query-shape coverage did not exercise executor scan TCBs"
if grep -q 'LOCK' <<<"$query_shape_output"; then
  fail "query-shape local-lite coverage hit RocksDB LOCK"
fi

aggregate_expr_output=$(
  printf "CREATE TABLE agg_expr_t(g INT, v INT);\nINSERT INTO agg_expr_t VALUES (1, 10), (1, 20), (2, 5), (NULL, 7), (NULL, 8);\nSELECT COUNT(*), SUM(v), MIN(v), MAX(v), AVG(v) FROM agg_expr_t;\nSELECT COUNT(*), SUM(v) + COUNT(*) FROM agg_expr_t WHERE g IS NULL;\nDROP TABLE agg_expr_t;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^ *5[[:space:]]+50[[:space:]]+5[[:space:]]+20[[:space:]]+10[[:space:]]*$' <<<"$aggregate_expr_output" ||
  fail "aggregate expression query did not return expected all-row aggregates"
grep -Eq '^ *2[[:space:]]+17[[:space:]]*$' <<<"$aggregate_expr_output" ||
  fail "aggregate expression query over NULL-filtered rows did not return expected result"
aggregate_expr_scan_count=$(grep -c 'LOCAL_LITE_SCAN_FULL' <<<"$aggregate_expr_output")
[[ "$aggregate_expr_scan_count" -ge 2 ]] ||
  fail "aggregate expression coverage did not exercise executor scan TCBs"
if grep -q 'LOCK' <<<"$aggregate_expr_output"; then
  fail "aggregate expression local-lite coverage hit RocksDB LOCK"
fi

grouped_aggregate_output=$(
  printf "CREATE TABLE agg_group_t(g INT, v INT);\nINSERT INTO agg_group_t VALUES (1, 10), (1, 20), (2, 5), (NULL, 7), (NULL, 8);\nSELECT g, COUNT(*), SUM(v) FROM agg_group_t GROUP BY g ORDER BY g;\nSELECT g, COUNT(*), SUM(v) FROM agg_group_t GROUP BY g HAVING SUM(v) >= 15 ORDER BY g;\nSELECT COUNT(*), SUM(v), MIN(v), MAX(v), AVG(v) FROM agg_group_t WHERE g = 1;\nCREATE TABLE agg_unique_t(g INT, v INT, UNIQUE(g));\nINSERT INTO agg_unique_t VALUES (1, 10), (2, 20), (NULL, 40), (NULL, 50);\nSELECT g, COUNT(*), SUM(v) FROM agg_unique_t GROUP BY g ORDER BY g;\nDROP TABLE agg_group_t;\nDROP TABLE agg_unique_t;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^ *1[[:space:]]+2[[:space:]]+30[[:space:]]*$' <<<"$grouped_aggregate_output" ||
  fail "grouped aggregate did not merge duplicate non-null group key"
grep -Eq '^ *2[[:space:]]+1[[:space:]]+5[[:space:]]*$' <<<"$grouped_aggregate_output" ||
  fail "grouped aggregate did not return singleton group key"
grep -Eq '^ *\?[[:space:]]+2[[:space:]]+15[[:space:]]*$' <<<"$grouped_aggregate_output" ||
  fail "grouped aggregate did not merge NULL group key"
grouped_having_count=$(grep -Ec '^ *(1|\?)[[:space:]]+2[[:space:]]+(30|15)[[:space:]]*$' <<<"$grouped_aggregate_output")
[[ "$grouped_having_count" -ge 4 ]] ||
  fail "grouped aggregate HAVING did not return expected qualifying groups"
grep -Eq '^ *2[[:space:]]+30[[:space:]]+10[[:space:]]+20[[:space:]]+15[[:space:]]*$' <<<"$grouped_aggregate_output" ||
  fail "aggregate over filtered grouped table did not return expected result"
grep -Eq '^ *\?[[:space:]]+2[[:space:]]+90[[:space:]]*$' <<<"$grouped_aggregate_output" ||
  fail "grouped aggregate over nullable UNIQUE key did not merge NULL rows"
grouped_aggregate_scan_count=$(grep -c 'LOCAL_LITE_SCAN_FULL' <<<"$grouped_aggregate_output")
[[ "$grouped_aggregate_scan_count" -ge 4 ]] ||
  fail "grouped aggregate coverage did not exercise executor scan TCBs"
if grep -q 'LOCK' <<<"$grouped_aggregate_output"; then
  fail "grouped aggregate local-lite coverage hit RocksDB LOCK"
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

compound_expr_output=$(
  printf "CREATE TABLE expr_t(a INT, b VARCHAR(20), c INT);\nINSERT INTO expr_t VALUES (1 + 4, 'ab' || 'cd', CASE WHEN 1 = 1 THEN 10 ELSE 20 END), (2, CAST('prefix' AS VARCHAR(20)), 30), (7, 'needle', 40);\nSELECT b FROM expr_t WHERE a BETWEEN 4 AND 6 AND c IN (10, 11);\nSELECT b FROM expr_t WHERE b LIKE 'pre%%';\nSELECT b FROM expr_t WHERE a = 7 OR c = 30;\nDROP TABLE expr_t;\nexit;\n" |
    run_sqlci
)
grep -q 'abcd' <<<"$compound_expr_output" ||
  fail "compound INSERT expressions did not persist concatenated CASE row"
grep -q 'prefix' <<<"$compound_expr_output" ||
  fail "LIKE predicate scan did not return CAST expression row"
grep -q 'needle' <<<"$compound_expr_output" ||
  fail "OR predicate scan did not return matching row"
compound_expr_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$compound_expr_output")
[[ "$compound_expr_selected_count" -ge 2 ]] ||
  fail "compound executor expression scans did not report one-row results"
grep -q -- '--- 2 row(s) selected.' <<<"$compound_expr_output" ||
  fail "OR predicate scan did not report two selected rows"

transaction_output=$(
  printf "CREATE TABLE tx(a INT, b VARCHAR(20));\nBEGIN WORK;\nINSERT INTO tx VALUES (1, 'rollback');\nSELECT b FROM tx WHERE a = 1;\nSELECT COUNT(*) FROM tx;\nROLLBACK WORK;\nSELECT b FROM tx WHERE a = 1;\nBEGIN WORK;\nINSERT INTO tx VALUES (2, 'commit');\nCOMMIT WORK;\nSELECT b FROM tx WHERE a = 2;\nDROP TABLE tx;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -q 'rollback' <<<"$transaction_output" ||
  fail "local transaction did not read its own pending insert"
grep -q 'commit' <<<"$transaction_output" ||
  fail "local transaction commit did not persist inserted row"
grep -q -- '--- 0 row(s) selected.' <<<"$transaction_output" ||
  fail "local transaction rollback did not discard inserted row"
transaction_selected_count=$(grep -c -- '--- 1 row(s) selected.' <<<"$transaction_output")
[[ "$transaction_selected_count" -ge 3 ]] ||
  fail "local transaction scans did not report expected selected rows"
transaction_full_scan_count=$(grep -c 'LOCAL_LITE_SCAN_FULL' <<<"$transaction_output")
[[ "$transaction_full_scan_count" -ge 4 ]] ||
  fail "transaction SELECT and COUNT queries did not use executor full scans"
grep -q 'LOCAL_LITE_SNAPSHOT_REUSE' <<<"$transaction_output" ||
  fail "separate SELECT statements did not reuse the transaction snapshot"
grep -q 'LOCAL_LITE_SNAPSHOT_RELEASE' <<<"$transaction_output" ||
  fail "transaction snapshot was not released by COMMIT/ROLLBACK"

atomic_primary_commit_output=$(
  printf "CREATE TABLE tx_atomic_pk(a INT PRIMARY KEY, b VARCHAR(32));\nINSERT INTO tx_atomic_pk VALUES (2, 'committed');\nBEGIN WORK;\nINSERT INTO tx_atomic_pk VALUES (1, 'must-not-commit'), (2, 'duplicate');\nCOMMIT WORK;\nexit;\n" |
    run_sqlci
)
grep -q 'duplicate local-lite primary key' <<<"$atomic_primary_commit_output" ||
  fail "transaction commit did not report the committed primary-key duplicate"

atomic_primary_verify_output=$(
  printf "SELECT b FROM tx_atomic_pk WHERE a = 1;\nSELECT b FROM tx_atomic_pk WHERE a = 2;\nDROP TABLE tx_atomic_pk;\nexit;\n" |
    run_sqlci
)
grep -q -- '--- 0 row(s) selected.' <<<"$atomic_primary_verify_output" ||
  fail "failed primary-key commit partially published an earlier row"
grep -q 'committed' <<<"$atomic_primary_verify_output" ||
  fail "failed primary-key commit damaged the existing committed row"

atomic_unique_commit_output=$(
  printf "CREATE TABLE tx_atomic_uq(a INT, b VARCHAR(32), UNIQUE(a));\nINSERT INTO tx_atomic_uq VALUES (2, 'committed');\nBEGIN WORK;\nINSERT INTO tx_atomic_uq VALUES (1, 'must-not-commit'), (2, 'duplicate');\nCOMMIT WORK;\nexit;\n" |
    run_sqlci
)
grep -q 'duplicate local-lite unique key' <<<"$atomic_unique_commit_output" ||
  fail "transaction commit did not report the committed UNIQUE duplicate"

atomic_unique_verify_output=$(
  printf "SELECT b FROM tx_atomic_uq WHERE a = 1;\nSELECT b FROM tx_atomic_uq WHERE a = 2;\nINSERT INTO tx_atomic_uq VALUES (3, 'after-failure');\nSELECT b FROM tx_atomic_uq WHERE a = 3;\nSELECT COUNT(*) FROM tx_atomic_uq;\nDROP TABLE tx_atomic_uq;\nexit;\n" |
    run_sqlci
)
grep -q -- '--- 0 row(s) selected.' <<<"$atomic_unique_verify_output" ||
  fail "failed UNIQUE commit partially published an earlier row"
grep -q 'committed' <<<"$atomic_unique_verify_output" ||
  fail "failed UNIQUE commit damaged the existing committed row"
grep -q 'after-failure' <<<"$atomic_unique_verify_output" ||
  fail "failed UNIQUE commit left keyless row-id metadata unusable"
grep -Eq '^ *2 *$' <<<"$atomic_unique_verify_output" ||
  fail "failed UNIQUE commit advanced keyless metadata or leaked a row"

autocommit_atomic_values_output=$(
  printf "CREATE TABLE autocommit_atomic_values(a INT PRIMARY KEY, b VARCHAR(32));\nINSERT INTO autocommit_atomic_values VALUES (2, 'committed');\nINSERT INTO autocommit_atomic_values VALUES (1, 'must-not-commit'), (2, 'duplicate');\nexit;\n" |
    run_sqlci
)
grep -q 'duplicate local-lite primary key' \
  <<<"$autocommit_atomic_values_output" ||
  fail "autocommit multi-row VALUES did not report the late primary-key conflict"

autocommit_atomic_values_verify_output=$(
  printf "SELECT b FROM autocommit_atomic_values WHERE a = 1;\nSELECT b FROM autocommit_atomic_values WHERE a = 2;\nDROP TABLE autocommit_atomic_values;\nexit;\n" |
    run_sqlci
)
grep -q -- '--- 0 row(s) selected.' \
  <<<"$autocommit_atomic_values_verify_output" ||
  fail "failed autocommit multi-row VALUES partially published an earlier row"
grep -q 'committed' <<<"$autocommit_atomic_values_verify_output" ||
  fail "failed autocommit multi-row VALUES damaged the existing committed row"

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

unique_key_output=$(
  printf "CREATE TABLE uq_t(a INT, b VARCHAR(20), UNIQUE(a));\nINSERT INTO uq_t VALUES (1, 'one');\nSELECT b FROM uq_t WHERE a = 1;\nINSERT INTO uq_t VALUES (1, 'dup');\nBEGIN WORK;\nINSERT INTO uq_t VALUES (2, 'two');\nSELECT b FROM uq_t WHERE a = 2;\nINSERT INTO uq_t VALUES (2, 'dup-pending');\nROLLBACK WORK;\nSELECT b FROM uq_t WHERE a = 2;\nDROP TABLE uq_t;\nexit;\n" |
    run_sqlci
)
grep -q 'one' <<<"$unique_key_output" ||
  fail "unique key table did not return inserted row"
grep -q 'two' <<<"$unique_key_output" ||
  fail "unique key transaction did not read pending row"
unique_duplicate_count=$(grep -c 'duplicate local-lite unique key' <<<"$unique_key_output")
[[ "$unique_duplicate_count" -ge 2 ]] ||
  fail "unique key duplicate diagnostics missing"
grep -q -- '--- 0 row(s) selected.' <<<"$unique_key_output" ||
  fail "unique key rollback did not discard pending row"

trace_setup_output=$(
  printf "CREATE TABLE pk_trace(a INT PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_trace VALUES (7, 'seven'), (8, 'eight');\nexit;\n" |
    run_sqlci
)
grep -q -- '--- 2 row(s) inserted.' <<<"$trace_setup_output" ||
  fail "trace setup table did not insert rows"

trace_pk_output=$(
  printf "SELECT b FROM pk_trace WHERE a = 7;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -q 'seven' <<<"$trace_pk_output" ||
  fail "primary-key trace query did not return row"
grep -q 'LOCAL_LITE_SCAN_GET_ROW' <<<"$trace_pk_output" ||
  fail "primary-key equality query did not use local-lite get-row scan"

trace_numeric_setup_output=$(
  printf "CREATE TABLE pk_num_trace(a NUMERIC(9,2) PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_num_trace VALUES (12.34, 'twelve'), (-0.50, 'minus');\nCREATE TABLE pk_dec_trace(a DECIMAL(5,2) PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_dec_trace VALUES (12.34, 'dpos'), (-0.50, 'dneg');\nCREATE TABLE pk_big_trace(a NUMERIC(30,2) PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_big_trace VALUES (1234567890123456789012345678.90, 'big');\nexit;\n" |
    run_sqlci
)
numeric_trace_insert_count=$(grep -c -- '--- .* row(s) inserted.' <<<"$trace_numeric_setup_output")
[[ "$numeric_trace_insert_count" -ge 3 ]] ||
  fail "numeric trace setup tables did not insert rows"

trace_numeric_pk_output=$(
  printf "SELECT b FROM pk_num_trace WHERE a = 12.34;\nSELECT b FROM pk_num_trace WHERE a = -0.50;\nSELECT b FROM pk_dec_trace WHERE a = 12.34;\nSELECT b FROM pk_dec_trace WHERE a = -0.50;\nSELECT b FROM pk_big_trace WHERE a = 1234567890123456789012345678.90;\nDROP TABLE pk_num_trace;\nDROP TABLE pk_dec_trace;\nDROP TABLE pk_big_trace;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -q 'twelve' <<<"$trace_numeric_pk_output" ||
  fail "numeric primary-key trace query did not return row"
grep -q 'minus' <<<"$trace_numeric_pk_output" ||
  fail "negative numeric primary-key trace query did not return row"
grep -q 'dpos' <<<"$trace_numeric_pk_output" ||
  fail "decimal primary-key trace query did not return row"
grep -q 'dneg' <<<"$trace_numeric_pk_output" ||
  fail "negative decimal primary-key trace query did not return row"
grep -q 'big' <<<"$trace_numeric_pk_output" ||
  fail "BigNum primary-key trace query did not return row"
numeric_get_row_count=$(grep -c 'LOCAL_LITE_SCAN_GET_ROW' <<<"$trace_numeric_pk_output")
[[ "$numeric_get_row_count" -ge 5 ]] ||
  fail "numeric/decimal/BigNum primary-key equality queries did not use local-lite get-row scan"

trace_unique_setup_output=$(
  printf "CREATE TABLE uq_trace(a INT, b VARCHAR(20), UNIQUE(a));\nINSERT INTO uq_trace VALUES (7, 'seven'), (8, 'eight');\nCREATE TABLE uq_num_trace(a NUMERIC(9,2), b VARCHAR(20), UNIQUE(a));\nINSERT INTO uq_num_trace VALUES (12.34, 'unum'), (-0.50, 'uneg');\nCREATE TABLE uq_dec_trace(a DECIMAL(5,2), b VARCHAR(20), UNIQUE(a));\nINSERT INTO uq_dec_trace VALUES (12.34, 'udpos'), (-0.50, 'udneg');\nexit;\n" |
    run_sqlci
)
unique_trace_insert_count=$(grep -c -- '--- 2 row(s) inserted.' <<<"$trace_unique_setup_output")
[[ "$unique_trace_insert_count" -ge 3 ]] ||
  fail "unique trace setup table did not insert rows"

trace_unique_output=$(
  printf "SELECT b FROM uq_trace WHERE a = 7;\nSELECT b FROM uq_trace WHERE a = 7 AND b = 'seven';\nSELECT b FROM uq_trace WHERE a = 7 AND b = 'miss';\nSELECT b FROM uq_num_trace WHERE a = 12.34;\nSELECT b FROM uq_num_trace WHERE a = -0.50;\nSELECT b FROM uq_dec_trace WHERE a = 12.34;\nSELECT b FROM uq_dec_trace WHERE a = -0.50;\nDROP TABLE uq_trace;\nDROP TABLE uq_num_trace;\nDROP TABLE uq_dec_trace;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -q 'seven' <<<"$trace_unique_output" ||
  fail "unique-key trace query did not return row"
unique_seven_count=$(grep -c 'seven' <<<"$trace_unique_output")
[[ "$unique_seven_count" -ge 2 ]] ||
  fail "unique-key get-row query with matching residual predicate did not return row"
grep -q -- '--- 0 row(s) selected.' <<<"$trace_unique_output" ||
  fail "unique-key get-row query did not preserve residual predicate filtering"
grep -q 'unum' <<<"$trace_unique_output" ||
  fail "numeric unique-key trace query did not return row"
grep -q 'uneg' <<<"$trace_unique_output" ||
  fail "negative numeric unique-key trace query did not return row"
grep -q 'udpos' <<<"$trace_unique_output" ||
  fail "decimal unique-key trace query did not return row"
grep -q 'udneg' <<<"$trace_unique_output" ||
  fail "negative decimal unique-key trace query did not return row"
unique_get_row_count=$(grep -c 'LOCAL_LITE_SCAN_GET_ROW' <<<"$trace_unique_output")
[[ "$unique_get_row_count" -ge 7 ]] ||
  fail "unique-key equality queries did not use local-lite get-row scan"

secondary_index_setup_output=$(
  printf "CREATE TABLE ix_trace(id INT PRIMARY KEY, code VARCHAR(8), label VARCHAR(20));\nINSERT INTO ix_trace VALUES (1, 'G', 'first'), (2, 'G', 'second'), (3, 'H', 'third');\nCREATE INDEX ix_trace_code ON ix_trace(code);\nCREATE UNIQUE INDEX ix_trace_label ON ix_trace(label);\nCREATE TABLE ix_null_trace(id INT PRIMARY KEY, code VARCHAR(8));\nINSERT INTO ix_null_trace VALUES (4, NULL), (5, NULL), (6, 'X');\nCREATE UNIQUE INDEX ix_null_trace_code ON ix_null_trace(code);\nCREATE TABLE ix_type_trace(id INT PRIMARY KEY, c CHAR(4), r REAL, d FLOAT(54), label VARCHAR(20));\nINSERT INTO ix_type_trace VALUES (1, 'A', -2.5, -20.5, 'negative'), (2, 'B', 0.0, 0.0, 'zero'), (3, 'C', 2.5, 20.5, 'positive');\nCREATE INDEX ix_type_trace_c ON ix_type_trace(c DESC);\nCREATE INDEX ix_type_trace_r ON ix_type_trace(r);\nCREATE INDEX ix_type_trace_d ON ix_type_trace(d DESC);\nexit;\n" |
    run_sqlci
)
grep -q -- '--- 3 row(s) inserted.' <<<"$secondary_index_setup_output" ||
  fail "secondary-index trace setup table did not insert rows"

secondary_index_trace_output=$(
  printf "SELECT id FROM ix_trace WHERE code = 'G' ORDER BY id;\nSELECT id FROM ix_trace WHERE label = 'third';\nSELECT id FROM ix_trace WHERE code > 'F' ORDER BY id;\nSELECT id FROM ix_null_trace WHERE code IS NULL ORDER BY id;\nSELECT label FROM ix_type_trace WHERE c >= 'B' ORDER BY id;\nSELECT label FROM ix_type_trace WHERE r < 0;\nSELECT label FROM ix_type_trace WHERE d > 0;\nSELECT label FROM ix_type_trace WHERE d = CAST(20.5 AS FLOAT(54));\nBEGIN WORK;\nUPDATE ix_trace SET code = 'Z' WHERE id = 1;\nSELECT id FROM ix_trace WHERE code = 'Z';\nROLLBACK WORK;\nSELECT id FROM ix_trace WHERE code = 'G' ORDER BY id;\nDROP TABLE ix_trace;\nDROP TABLE ix_null_trace;\nDROP TABLE ix_type_trace;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^ *1 *$' <<<"$secondary_index_trace_output" ||
  fail "secondary-index equality query did not return the first row"
grep -Eq '^ *2 *$' <<<"$secondary_index_trace_output" ||
  fail "non-unique secondary-index equality query did not return all rows"
grep -Eq '^ *3 *$' <<<"$secondary_index_trace_output" ||
  fail "unique secondary-index equality query did not return its row"
grep -Eq '^ *4 *$' <<<"$secondary_index_trace_output" ||
  fail "nullable unique-index lookup did not return its first NULL row"
grep -Eq '^ *5 *$' <<<"$secondary_index_trace_output" ||
  fail "nullable unique-index lookup did not return its second NULL row"
grep -q 'negative' <<<"$secondary_index_trace_output" ||
  fail "REAL secondary-index range lookup did not return its row"
grep -q 'zero' <<<"$secondary_index_trace_output" ||
  fail "CHAR secondary-index range lookup did not return its first row"
grep -q 'positive' <<<"$secondary_index_trace_output" ||
  fail "CHAR/FLOAT(54) secondary-index range lookups did not return rows"
secondary_index_scan_count=$(
  grep -c 'LOCAL_LITE_SCAN_INDEX_EQ' <<<"$secondary_index_trace_output"
)
[[ "$secondary_index_scan_count" -ge 5 ]] ||
  fail "secondary-index equality queries did not use local-lite index scans"
grep -q 'LOCAL_LITE_SCAN_INDEX_BOUNDED' <<<"$secondary_index_trace_output" ||
  fail "secondary-index range query did not use bounded local-lite index scan"
grep -q 'LOCAL_LITE_INDEX_BOUNDS index=IX_TRACE_CODE' <<<"$secondary_index_trace_output" ||
  fail "optimizer did not select the code secondary index"
grep -q 'LOCAL_LITE_INDEX_BOUNDS index=IX_TRACE_LABEL' <<<"$secondary_index_trace_output" ||
  fail "optimizer did not select the unique label secondary index"
grep -Eq 'LOCAL_LITE_INDEX_ONLY covering=[1-9][0-9]* base_lookups=0' <<<"$secondary_index_trace_output" ||
  fail "current secondary-index records did not use covering RocksDB values"
grep -Eq 'LOCAL_LITE_INDEX_BOUNDS .*candidates=2' <<<"$secondary_index_trace_output" ||
  fail "nullable unique-index lookup did not scan its RocksDB NULL-key range"

trace_full_output=$(
  printf "SELECT a FROM pk_trace WHERE b = 'seven';\nDROP TABLE pk_trace;\nexit;\n" |
    run_sqlci_trace_scan 2>&1
)
grep -Eq '^ *7 *$' <<<"$trace_full_output" ||
  fail "non-key trace query did not return row"
grep -q 'LOCAL_LITE_SCAN_FULL' <<<"$trace_full_output" ||
  fail "non-key predicate query did not fall back to full executor scan"

explain_output=$(
  printf "CONTROL QUERY DEFAULT GENERATE_EXPLAIN 'ON';\nCREATE TABLE pk_plan(a INT PRIMARY KEY, b VARCHAR(20));\nINSERT INTO pk_plan VALUES (7, 'seven');\nPREPARE xx FROM SELECT b FROM pk_plan WHERE a = 7;\nSELECT operator, description FROM TABLE(EXPLAIN(NULL, 'XX'));\nCREATE TABLE uq_plan(a INT, b VARCHAR(20), UNIQUE(a));\nINSERT INTO uq_plan VALUES (7, 'seven');\nPREPARE xx FROM SELECT b FROM uq_plan WHERE a = 7 AND b = 'seven';\nSELECT operator, description FROM TABLE(EXPLAIN(NULL, 'XX'));\nCREATE TABLE keyless_plan(g INT, v INT);\nINSERT INTO keyless_plan VALUES (1, 10), (1, 20), (2, 5);\nPREPARE xx FROM SELECT g, COUNT(*), SUM(v) FROM keyless_plan GROUP BY g;\nSELECT operator, description FROM TABLE(EXPLAIN(NULL, 'XX'));\nDROP TABLE pk_plan;\nDROP TABLE uq_plan;\nDROP TABLE keyless_plan;\nexit;\n" |
    run_sqlci
)
grep -q 'TRAFODION_SCAN' <<<"$explain_output" ||
  fail "local-lite explain did not include Trafodion scan operator"
grep -q 'scan_type: subset scan of table TRAFODION.SEABASE.PK_PLAN' <<<"$explain_output" ||
  fail "primary-key equality explain did not show subset scan"
grep -q 'scan_type: subset scan of table TRAFODION.SEABASE.UQ_PLAN' <<<"$explain_output" ||
  fail "unique-key equality explain did not show subset scan"
grep -q 'key_columns: A' <<<"$explain_output" ||
  fail "local-lite key equality explain did not expose key column"
grep -q 'executor_predicates: (B =' <<<"$explain_output" ||
  fail "unique-key equality explain did not preserve residual executor predicate"
grep -q 'probes: 1' <<<"$explain_output" ||
  fail "local-lite key equality explain did not show single-probe access"
grep -q 'key_columns: SYSKEY' <<<"$explain_output" ||
  fail "keyless local-lite explain did not expose synthetic SYSKEY metadata"
grep -Eq 'HASH_GROUPBY|SORT_GROUPBY' <<<"$explain_output" ||
  fail "keyless grouped aggregate plan eliminated the required groupby"

secondary_explain_output=$(
  printf "CONTROL QUERY DEFAULT GENERATE_EXPLAIN 'ON';\nCREATE TABLE ix_plan(id INT PRIMARY KEY, a INT, payload VARCHAR(20));\nINSERT INTO ix_plan VALUES (1, 10, 'covered');\nCREATE INDEX ix_plan_a ON ix_plan(a);\nPREPARE ix FROM SELECT payload FROM ix_plan WHERE a = 10;\nSELECT operator, description FROM TABLE(EXPLAIN(NULL, 'IX'));\nDROP TABLE ix_plan;\nexit;\n" |
    run_sqlci
)
grep -q 'local_lite_storage: rocksdb' <<<"$secondary_explain_output" ||
  fail "secondary-index explain did not identify RocksDB storage"
grep -q 'secondary_index: yes index_only: yes' <<<"$secondary_explain_output" ||
  fail "secondary-index explain did not expose index-only access"

catalog_ddl_output=$(
  printf "CREATE TABLE catalog_base(a INT, b VARCHAR(20));\nINSERT INTO catalog_base VALUES (1, 'one');\nCREATE VIEW v AS SELECT * FROM catalog_base;\nALTER TABLE catalog_base ADD COLUMN c INT;\nTRUNCATE TABLE catalog_base;\nCREATE TABLE constrained(a INT CHECK (a > 0));\nINSERT INTO constrained VALUES (0);\nINSERT INTO constrained VALUES (1);\nDROP VIEW v;\nDROP TABLE constrained;\nDROP TABLE catalog_base;\nexit;\n" |
    run_sqlci
)
catalog_ddl_complete_count=$(grep -c -- '--- SQL operation complete.' <<<"$catalog_ddl_output")
[[ "$catalog_ddl_complete_count" -ge 5 ]] ||
  fail "M4 catalog DDL did not complete all supported operations"
grep -q 'ERROR\[8101\]' <<<"$catalog_ddl_output" ||
  fail "CHECK constraint violation diagnostic missing"
grep -q -- '--- 1 row(s) inserted.' <<<"$catalog_ddl_output" ||
  fail "CHECK constraint did not allow a valid row"

echo "local-lite RocksDB sqlci smoke passed"
