#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci="$repo_root/core/sql/lib/linux/64bit/debug/sqlci"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"
store_dir=$(mktemp -d /tmp/traf-local-lite-m16d.XXXXXX)
trap 'rm -rf "$store_dir"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$sqlci" ]] || fail "missing built sqlci: $sqlci"

run_sqlci() {
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_LITE_TRACE_SCAN=1 TRAF_LOCAL_LITE_TRACE_SNAPSHOT=1 \
    TRAF_LOCAL_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" "$sqlci"
}

output=$(
  printf '%s\n' \
    "CREATE TABLE TPCC_STOCK(S_W_ID INT NOT NULL,S_I_ID INT NOT NULL,S_QUANTITY INT NOT NULL,PRIMARY KEY(S_W_ID,S_I_ID));" \
    "CREATE TABLE TPCC_ORDER_LINE(OL_W_ID INT NOT NULL,OL_D_ID INT NOT NULL,OL_O_ID INT NOT NULL,OL_NUMBER INT NOT NULL,OL_I_ID INT NOT NULL,OL_SUPPLY_W_ID INT NOT NULL,PRIMARY KEY(OL_W_ID,OL_D_ID,OL_O_ID,OL_NUMBER));" \
    "CREATE INDEX TPCC_ORDER_LINE_STOCK_IX ON TPCC_ORDER_LINE(OL_W_ID,OL_D_ID,OL_O_ID,OL_SUPPLY_W_ID,OL_I_ID);" \
    "INSERT INTO TPCC_STOCK VALUES (1,11,4),(1,12,40),(2,11,3);" \
    "INSERT INTO TPCC_ORDER_LINE VALUES (1,1,81,1,11,1),(1,1,81,2,12,1),(1,1,82,1,11,1),(1,1,83,1,11,2),(1,1,84,1,12,1);" \
    "SELECT COUNT(DISTINCT L.OL_I_ID) FROM TPCC_ORDER_LINE L JOIN TPCC_STOCK S ON L.OL_SUPPLY_W_ID=S.S_W_ID AND L.OL_I_ID=S.S_I_ID WHERE L.OL_W_ID=1 AND L.OL_D_ID=1 AND L.OL_O_ID>=81 AND L.OL_O_ID<84 AND S.S_QUANTITY<10;" \
    "SELECT OL_SUPPLY_W_ID,OL_I_ID FROM TPCC_ORDER_LINE WHERE OL_W_ID=1 AND OL_D_ID=1 AND OL_O_ID>=81 AND OL_O_ID<84 ORDER BY OL_SUPPLY_W_ID,OL_I_ID;" \
    "CONTROL QUERY DEFAULT GENERATE_EXPLAIN 'ON';" \
    "PREPARE M16 FROM SELECT OL_SUPPLY_W_ID,OL_I_ID FROM TPCC_ORDER_LINE WHERE OL_W_ID=1 AND OL_D_ID=1 AND OL_O_ID>=81 AND OL_O_ID<84;" \
    "SELECT operator,description FROM TABLE(EXPLAIN(NULL,'M16'));" \
    "DROP TABLE TPCC_ORDER_LINE;" \
    "DROP TABLE TPCC_STOCK;" \
    "exit;" | run_sqlci 2>&1
)

grep -Eq -- '--- 1 row\(s\) selected\.' <<<"$output" ||
  fail "join-equivalent Stock-Level query did not return one row"
grep -Eq '^ *1 *$' <<<"$output" ||
  fail "join-equivalent Stock-Level result was not one"
grep -q 'LOCAL_LITE_SCAN_PRIMARY_RANGE table=.*TPCC_ORDER_LINE_STOCK_IX' <<<"$output" ||
  fail "ordered Stock-Level index was not selected"
grep -q 'secondary_index: yes' <<<"$output" ||
  fail "Stock-Level explain did not show secondary index access"
grep -q 'index_only: yes' <<<"$output" ||
  fail "Stock-Level explain did not show index-only access"
echo "LocalLite M16D optimizer range and join-equivalence checks passed"
