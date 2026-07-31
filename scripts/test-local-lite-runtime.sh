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
generator_makefile="$repo_root/core/sql/nskgmake/generator/Makefile"
sqlcmd_source="$repo_root/core/sql/sqlci/SqlCmd.cpp"
local_sql_handler="$repo_root/core/sql/sqlci/LocalLiteSqlTable.cpp"
local_sqlci_smoke="$repo_root/scripts/test-local-lite-rocksdb-sqlci.sh"
local_store_concurrency="$repo_root/scripts/test-local-lite-store-concurrency.sh"
local_store_process_boundary="$repo_root/scripts/test-local-lite-store-process-boundary.sh"
local_statement_snapshot="$repo_root/scripts/test-local-lite-statement-snapshot.sh"
local_transaction_snapshot="$repo_root/scripts/test-local-lite-transaction-snapshot.sh"
local_regress_dir="$repo_root/core/sql/regress/localLite"
local_regress="$local_regress_dir/runregr"
storage_stubs="$repo_root/core/sql/executor/LocalLiteStorageStubs.cpp"
executor_root="$repo_root/core/sql/executor/ex_root.cpp"
localstore_dir="$repo_root/core/sql/localstore"
localstore_header="$localstore_dir/LocalLiteRocksDBStore.h"
localstore_source="$localstore_dir/LocalLiteRocksDBStore.cpp"
localstore_codec_header="$localstore_dir/LocalLiteRowCodec.h"
localstore_codec_source="$localstore_dir/LocalLiteRowCodec.cpp"
cmp_stmt="$repo_root/core/sql/arkcmp/CmpStatement.cpp"
cmp_ddl_common="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLcommon.cpp"
cmp_ddl_table="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLtable.cpp"
gen_precode="$repo_root/core/sql/generator/GenPreCode.cpp"
gen_relmisc="$repo_root/core/sql/generator/GenRelMisc.cpp"
ex_ddl="$repo_root/core/sql/executor/ex_ddl.cpp"
ex_transaction="$repo_root/core/sql/executor/ex_transaction.cpp"

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
grep -q 'LocalLiteRowCodec.cpp' "$executor_makefile" || fail "executor build must compile the local-lite row codec"
grep -q 'LocalLiteRocksDBStore.cpp' "$generator_makefile" ||
  fail "generator build must compile local-lite store metadata access"
grep -q 'LocalLiteRowCodec.cpp' "$generator_makefile" ||
  fail "generator build must compile local-lite primary key codec"
grep -q 'LocalLiteUnsupportedHbaseTcb' "$storage_stubs" ||
  fail "local-lite HBase access build must create an executor TCB instead of returning NULL"
grep -q 'LocalLiteHbaseScanTcb' "$storage_stubs" ||
  fail "local-lite SELECT must use an executor scan TCB"
grep -q 'LocalLiteHbaseInsertTcb' "$storage_stubs" ||
  fail "local-lite INSERT must use an executor insert TCB"
grep -q 'listOfGetRows' "$storage_stubs" ||
  fail "local-lite executor scan must consume optimized get-row requests"
grep -q 'LocalLiteTxn::getRowByKey' "$localstore_source" ||
  fail "local-lite get-row access must go through the transaction facade"
grep -q 'beginStatement(this, transactionId)' "$localstore_source" ||
  fail "explicit local transactions must own a persistent read context"
grep -q 'readOwner = this' "$localstore_source" ||
  fail "transaction scans must resolve through the transaction snapshot"
grep -q 'commitPendingRows' "$localstore_source" ||
  fail "transaction commit must publish each table through one storage batch"
grep -q 'rocksdb_write(db, writeOptions, batch' "$localstore_source" ||
  fail "local-lite table publication must use a RocksDB write batch"
grep -q 'failed primary-key commit partially published' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must cover atomic primary-key commit failure"
grep -q 'failed UNIQUE commit partially published' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must cover atomic UNIQUE commit failure"
grep -q 'LocalLiteTxn txn' "$storage_stubs" ||
  fail "local-lite executor scan/insert TCBs must use the transaction facade"
grep -q 'getExecutionCount' "$storage_stubs" ||
  fail "local-lite executor scans must bind to the current statement execution"
grep -q 'LocalLiteTxnManager::beginStatement' "$executor_root" ||
  fail "executor root must begin the local-lite statement snapshot context"
grep -q 'LocalLiteTxnManager::endStatement' "$executor_root" ||
  fail "executor root must release the local-lite statement snapshot context"
if grep -q 'allocateRowId' "$localstore_header" ||
   grep -q 'LocalLiteRocksDBStore::allocateRowId' "$localstore_source" ||
   grep -q 'LocalLiteRocksDBStore::putRow' "$localstore_source"; then
  fail "local-lite store must not expose legacy row-id allocation or direct row put APIs"
fi
grep -q 'TRAF_LOCAL_LITE_TRACE_SCAN' "$storage_stubs" ||
  fail "local-lite executor scan must expose test-only scan path tracing"
grep -q 'LocalLiteProjectBinaryRow' "$storage_stubs" ||
  fail "local-lite executor scan must project from binary persisted rows"
grep -q 'LocalLiteNormalizeBinaryRow' "$storage_stubs" ||
  fail "local-lite executor insert must persist normalized executor binary aligned rows"
grep -q 'ExpTupleDesc::getVarOffset' "$localstore_codec_source" ||
  fail "local-lite insert normalization must resolve indirect aligned VARCHAR offsets through the VOA"
grep -q 'uniqueKeyColumns' "$localstore_header" ||
  fail "local-lite table metadata must preserve UNIQUE key columns"
grep -q 'LocalLiteBuildUniqueKey' "$localstore_source" ||
  fail "local-lite insert path must build UNIQUE secondary keys"
grep -q 'beginForExecutor' "$localstore_header" ||
  fail "local-lite transaction manager must expose executor-bound begin"
grep -q 'commitForExecutor' "$localstore_header" ||
  fail "local-lite transaction manager must expose executor-bound commit"
grep -q 'rollbackForExecutor' "$localstore_header" ||
  fail "local-lite transaction manager must expose executor-bound rollback"
grep -q 'currentExecutorTxnId' "$localstore_header" ||
  fail "local-lite transaction manager must expose current executor transaction id"
grep -q 'LocalLiteTxnManager::beginForExecutor' "$ex_transaction" ||
  fail "transaction TCB must begin local-lite transactions through executor-bound facade"
grep -q 'LocalLiteTxnManager::commitForExecutor' "$ex_transaction" ||
  fail "transaction TCB must commit local-lite transactions through executor-bound facade"
grep -q 'LocalLiteTxnManager::rollbackForExecutor' "$ex_transaction" ||
  fail "transaction TCB must rollback local-lite transactions through executor-bound facade"
if grep -q 'LocalLiteDecodeFields' "$storage_stubs"; then
  fail "local-lite executor scan must not decode SQLCI text field rows"
fi
grep -q 'LocalLiteSqlTable.cpp' "$sqlcilib_makefile" ||
  fail "sqlcilib build must compile the local-lite SQL table handler"
if grep -q 'processInsert' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept INSERT after executor insert migration"
fi
if grep -q 'LocalLiteEncodeBinaryRow' "$local_sql_handler"; then
  fail "SQLCI handler must not encode INSERT rows after executor insert migration"
fi
grep -q 'LocalLiteRocksDBStore.cpp' "$sqlcomp_makefile" ||
  fail "sqlcomp build must compile the RocksDB local store"
grep -q 'localstore' "$sqlcomp_makefile" ||
  fail "sqlcomp build must include the localstore source path"
grep -q 'LocalLiteSqlTable_process' "$sqlcmd_source" ||
  fail "DML processing must route local-lite table statements before CLI prepare"
[[ -f "$local_sql_handler" ]] || fail "missing local-lite SQL table handler: $local_sql_handler"
[[ -f "$local_sqlci_smoke" ]] || fail "missing local-lite RocksDB SQLCI smoke test: $local_sqlci_smoke"
[[ -x "$local_store_concurrency" ]] ||
  fail "missing executable local-lite store concurrency test: $local_store_concurrency"
[[ -x "$local_store_process_boundary" ]] ||
  fail "missing executable local-lite store process-boundary test: $local_store_process_boundary"
[[ -x "$local_statement_snapshot" ]] ||
  fail "missing executable local-lite statement snapshot test: $local_statement_snapshot"
[[ -x "$local_transaction_snapshot" ]] ||
  fail "missing executable local-lite transaction snapshot test: $local_transaction_snapshot"
[[ -x "$local_regress" ]] ||
  fail "missing executable local-lite regress runner: $local_regress"
[[ -x "$local_regress_dir/FILTER" ]] ||
  fail "missing executable local-lite regress output filter"
for test_number in 001 002 003 004 005 006 007 008 009 010 011 012 013 014 015 016; do
  [[ -f "$local_regress_dir/TEST$test_number" &&
     -f "$local_regress_dir/EXPECTED$test_number" ]] ||
    fail "local-lite regress lane is missing TEST/EXPECTED$test_number"
done
grep -q 'TRAF_LOCAL_STORE_DIR' "$local_regress" ||
  fail "local-lite regress cases must use isolated RocksDB stores"
grep -q 'MXID' "$local_regress_dir/FILTER" ||
  fail "local-lite regress output must normalize dynamic diagnostic ids"
grep -q 'local-lite-regress' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the local-lite regress lane"
[[ -f "$localstore_header" ]] || fail "missing local store header: $localstore_header"
[[ -f "$localstore_source" ]] || fail "missing local store source: $localstore_source"
grep -q 'LocalLiteBuildPrimaryKeyFromTextFields' "$localstore_codec_header" ||
  fail "local-lite row codec must expose compiler primary-key literal encoding"
grep -q 'LocalLiteBuildPrimaryKeyFromTextFields' "$localstore_codec_source" ||
  fail "local-lite row codec must build primary keys from compiler literals"
grep -q 'TRAF_LOCAL_STORE_DIR' "$localstore_source" || fail "local store must support TRAF_LOCAL_STORE_DIR override"
grep -q 'already open by another' "$localstore_source" ||
  fail "local store must explain cross-process RocksDB lock failures"
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
if grep -q 'processSelect' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept SELECT after executor scan migration"
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
grep -q 'localLiteRewritePrimaryGetRows' "$gen_precode" ||
  fail "generator pre-code must map primary-key equality to local-lite get-row keys"
grep -Fq 'table.primaryKeyColumns[i]' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "local-lite NATable must expose real primary-key column ordinals"
grep -Fq 'table.uniqueKeyColumns' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "local-lite NATable must expose UNIQUE key metadata"
grep -Fq 'setUnique(TRUE)' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "local-lite NATable UNIQUE metadata must create unique access paths"
grep -q 'LOCAL_LITE_SCAN_GET_ROW' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must verify primary-key equality uses get-row scan"
grep -q 'LOCAL_LITE_SCAN_FULL' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must verify non-key predicates fall back to full scan"
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
