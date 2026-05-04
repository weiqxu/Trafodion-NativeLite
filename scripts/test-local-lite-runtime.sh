#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqstart="$repo_root/core/sqf/sql/scripts/sqstart"
sqenvcom="$repo_root/core/sqf/sqenvcom.sh"
deps="$repo_root/scripts/install-local-lite-deps.sh"
makerules="$repo_root/core/sql/nskgmake/Makerules.linux"
executor_makefile="$repo_root/core/sql/nskgmake/executor/Makefile"
sqlcilib_makefile="$repo_root/core/sql/nskgmake/sqlcilib/Makefile"
sqlcomp_makefile="$repo_root/core/sql/nskgmake/sqlcomp/Makefile"
sqlcmd_source="$repo_root/core/sql/sqlci/SqlCmd.cpp"
local_sql_handler="$repo_root/core/sql/sqlci/LocalLiteSqlTable.cpp"
local_sqlci_smoke="$repo_root/scripts/test-local-lite-rocksdb-sqlci.sh"
storage_stubs="$repo_root/core/sql/executor/LocalLiteStorageStubs.cpp"
localstore_dir="$repo_root/core/sql/localstore"
localstore_header="$localstore_dir/LocalLiteRocksDBStore.h"
localstore_source="$localstore_dir/LocalLiteRocksDBStore.cpp"
cmp_stmt="$repo_root/core/sql/arkcmp/CmpStatement.cpp"
cmp_ddl_common="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLcommon.cpp"
cmp_ddl_table="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLtable.cpp"
gen_precode="$repo_root/core/sql/generator/GenPreCode.cpp"
gen_relmisc="$repo_root/core/sql/generator/GenRelMisc.cpp"
ex_ddl="$repo_root/core/sql/executor/ex_ddl.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -q 'TRAF_LOCAL_LITE' "$sqstart" || fail "sqstart must gate Hadoop/HBase startup checks on TRAF_LOCAL_LITE"
grep -q 'Skipping ZooKeeper cleanup and HBase availability check for local-lite' "$sqstart" ||
  fail "sqstart must document the local-lite skip in startup output"

grep -q 'TRAF_LOCAL_LITE' "$sqenvcom" || fail "sqenvcom must gate Hadoop/HBase classpath setup on TRAF_LOCAL_LITE"
grep -q 'Skipping Hadoop/HBase classpath setup for local-lite' "$sqenvcom" ||
  fail "sqenvcom must document the local-lite classpath skip"
grep -q 'SQ_CLASSPATH=' "$sqenvcom" || fail "sqenvcom must clear SQ_CLASSPATH for local-lite"

grep -q 'hbcheck 4 30' "$sqstart" || fail "default sqstart path must still run hbcheck"
grep -q 'cleanZKNodes' "$sqstart" || fail "default sqstart path must still clean ZooKeeper nodes"

grep -q 'librocksdb-dev' "$deps" || fail "apt local-lite dependencies must include RocksDB headers"
grep -q 'rocksdb-devel' "$deps" || fail "rpm local-lite dependencies must include RocksDB headers"
grep -q 'ROCKSDB_LIB' "$makerules" || fail "SQL makerules must define RocksDB link flags"
grep -q -- '-lrocksdb' "$makerules" || fail "local-lite SQL link flags must include librocksdb"
grep -q 'localstore' "$executor_makefile" || fail "executor build must include the localstore source path"
grep -q 'LocalLiteRocksDBStore.cpp' "$executor_makefile" || fail "executor build must compile the RocksDB local store"
grep -q 'LocalLiteUnsupportedHbaseTcb' "$storage_stubs" ||
  fail "local-lite HBase access build must create an executor TCB instead of returning NULL"
grep -q 'LocalLiteSqlTable.cpp' "$sqlcilib_makefile" ||
  fail "sqlcilib build must compile the local-lite SQL table handler"
grep -q 'LocalLiteRocksDBStore.cpp' "$sqlcomp_makefile" ||
  fail "sqlcomp build must compile the RocksDB local store"
grep -q 'localstore' "$sqlcomp_makefile" ||
  fail "sqlcomp build must include the localstore source path"
grep -q 'LocalLiteSqlTable_process' "$sqlcmd_source" ||
  fail "DML processing must route local-lite table statements before CLI prepare"
[[ -f "$local_sql_handler" ]] || fail "missing local-lite SQL table handler: $local_sql_handler"
[[ -f "$local_sqlci_smoke" ]] || fail "missing local-lite RocksDB SQLCI smoke test: $local_sqlci_smoke"
[[ -f "$localstore_header" ]] || fail "missing local store header: $localstore_header"
[[ -f "$localstore_source" ]] || fail "missing local store source: $localstore_source"
grep -q 'TRAF_LOCAL_STORE_DIR' "$localstore_source" || fail "local store must support TRAF_LOCAL_STORE_DIR override"
grep -q 'localstore/rocksdb' "$localstore_source" || fail "local store must default under TRAF_VAR/localstore/rocksdb"
if grep -q 'processCreate' "$local_sql_handler"; then
  fail "SQLCI handler must not parse CREATE TABLE after compiler DDL migration"
fi
if grep -q 'processDrop' "$local_sql_handler"; then
  fail "SQLCI handler must not parse DROP TABLE after compiler DDL migration"
fi
if grep -q 'startsWithWord(sql, "CREATE TABLE")' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept CREATE TABLE after compiler DDL migration"
fi
if grep -q 'startsWithWord(sql, "DROP TABLE")' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept DROP TABLE after compiler DDL migration"
fi
grep -q 'executeSeabaseDDL(localLiteDDLExpr, boundLocalDDL' "$cmp_stmt" ||
  fail "embedded compiler DDL path must dispatch local table DDL directly"
grep -q 'localLiteLocalTableDDL' "$cmp_ddl_common" ||
  fail "Seabase DDL common path must identify local-lite local table DDL"
grep -q '(NOT localLiteLocalTableDDL) && sendAllControlsAndFlags()' "$cmp_ddl_common" ||
  fail "local-lite local table DDL must skip sendAllControlsAndFlags"
grep -q 'startXn = FALSE' "$cmp_ddl_common" ||
  fail "local-lite local table DDL must disable DDL transaction start"
grep -q 'localLiteCreateTable' "$cmp_ddl_table" ||
  fail "CREATE TABLE must route to the local RocksDB catalog from sqlcomp"
grep -q 'localLiteDropTable' "$cmp_ddl_table" ||
  fail "DROP TABLE must route to the local RocksDB catalog from sqlcomp"
grep -q 'xnNeeded() = FALSE' "$gen_precode" ||
  fail "generator pre-code must mark local-lite CREATE/DROP as no transaction"
grep -q 'ddl_tdb->setHbaseDDL(TRUE)' "$gen_relmisc" ||
  fail "generator must route local-lite CREATE/DROP through PROCESSDDL"
grep -q 'switchToCmpContext' "$ex_ddl" ||
  fail "executor DDL path must initialize embedded arkcmp for local-lite"

sq_classpath=$(
  cd "$repo_root/core/sqf"
  TRAF_LOCAL_LITE=1 TRAF_CLUSTER_ID=1 TRAF_INSTANCE_ID=1 bash -lc \
    'source ./sqenv.sh >/dev/null 2>/dev/null; printf "%s" "$SQ_CLASSPATH"'
)

[[ -z "$sq_classpath" ]] || fail "local-lite SQ_CLASSPATH must be empty, got: $sq_classpath"

echo "local-lite runtime script checks passed"
