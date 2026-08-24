#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqstart="$repo_root/core/sqf/sql/scripts/sqstart"
sqenvcom="$repo_root/core/sqf/sqenvcom.sh"
deps="$repo_root/scripts/install-lite-deps.sh"
makerules="$repo_root/core/sql/nskgmake/Makerules.linux"
executor_makefile="$repo_root/core/sql/nskgmake/executor/Makefile"
sqlcilib_makefile="$repo_root/core/sql/nskgmake/sqlcilib/Makefile"
sqlcomp_makefile="$repo_root/core/sql/nskgmake/sqlcomp/Makefile"
generator_makefile="$repo_root/core/sql/nskgmake/generator/Makefile"
sqlcmd_source="$repo_root/core/sql/sqlci/SqlCmd.cpp"
local_sql_handler="$repo_root/core/sql/sqlci/LiteSqlTable.cpp"
local_sqlci_smoke="$repo_root/scripts/test-lite-rocksdb-sqlci.sh"
local_store_concurrency="$repo_root/scripts/test-lite-store-concurrency.sh"
local_store_process_boundary="$repo_root/scripts/test-lite-store-process-boundary.sh"
local_statement_snapshot="$repo_root/scripts/test-lite-statement-snapshot.sh"
local_transaction_snapshot="$repo_root/scripts/test-lite-transaction-snapshot.sh"
local_session_transactions="$repo_root/scripts/test-lite-session-transactions.sh"
local_context_transactions="$repo_root/scripts/test-lite-context-transactions.sh"
local_server_test="$repo_root/scripts/test-lite-server.sh"
local_t4jdbc_test="$repo_root/scripts/test-lite-t4jdbc.sh"
local_t4jdbc_source="$repo_root/scripts/TrafodionLiteT4JdbcTest.java"
local_row_codec_test_stubs="$repo_root/scripts/lite-row-codec-test-stubs.cpp"
local_regress_dir="$repo_root/core/sql/regress/lite"
local_regress="$local_regress_dir/runregr"
local_legacy_regress_dir="$repo_root/core/sql/regress/liteLegacy"
local_legacy_manifest="$local_legacy_regress_dir/manifest.tsv"
local_legacy_extra_manifest="$local_legacy_regress_dir/standard-extra-manifest.tsv"
local_newregr_manifest="$local_legacy_regress_dir/newregr-inventory.tsv"
local_m2_m6_reprobe="$local_legacy_regress_dir/reprobe-m2-m6-2026-08-12.tsv"
local_legacy_regress="$local_legacy_regress_dir/runregr"
local_legacy_audit="$repo_root/scripts/audit-lite-legacy-regress.sh"
local_upstream_audit="$repo_root/scripts/audit-lite-upstream-regress.sh"
local_legacy_m10="$repo_root/scripts/test-lite-legacy-convergence.sh"
local_metadata="$repo_root/scripts/test-lite-metadata.sh"
storage_stubs="$repo_root/core/sql/executor/LiteStorageStubs.cpp"
executor_root="$repo_root/core/sql/executor/ex_root.cpp"
litestore_dir="$repo_root/core/sql/litestore"
litestore_header="$litestore_dir/LiteRocksDBStore.h"
litestore_source="$litestore_dir/LiteRocksDBStore.cpp"
litestore_codec_header="$litestore_dir/LiteRowCodec.h"
litestore_codec_source="$litestore_dir/LiteRowCodec.cpp"
cmp_stmt="$repo_root/core/sql/arkcmp/CmpStatement.cpp"
cmp_ddl_common="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLcommon.cpp"
cmp_ddl_table="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLtable.cpp"
cmp_ddl_view="$repo_root/core/sql/sqlcomp/CmpSeabaseDDLview.cpp"
gen_precode="$repo_root/core/sql/generator/GenPreCode.cpp"
gen_relmisc="$repo_root/core/sql/generator/GenRelMisc.cpp"
ex_ddl="$repo_root/core/sql/executor/ex_ddl.cpp"
ex_ctas="$repo_root/core/sql/executor/ExExeUtilLoad.cpp"
ex_transaction="$repo_root/core/sql/executor/ex_transaction.cpp"
context_header="$repo_root/core/sql/cli/Context.h"
context_source="$repo_root/core/sql/cli/Context.cpp"
transaction_header="$repo_root/core/sql/executor/ex_transaction.h"
server_source="$repo_root/core/sql/bin/TrafodionLiteServerMain.cpp"
server_makefile="$repo_root/core/sql/nskgmake/trafodion_lite_server/Makefile"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -q 'TRAF_LITE' "$sqstart" || fail "sqstart must gate Hadoop/HBase startup checks on TRAF_LITE"
grep -q 'Skipping ZooKeeper cleanup and HBase availability check for lite' "$sqstart" ||
  fail "sqstart must document the lite skip in startup output"

grep -q 'TRAF_LITE' "$sqenvcom" || fail "sqenvcom must gate Hadoop/HBase classpath setup on TRAF_LITE"
grep -q 'Skipping Hadoop/HBase classpath setup for lite' "$sqenvcom" ||
  fail "sqenvcom must document the lite classpath skip"
grep -q 'SQ_CLASSPATH=' "$sqenvcom" || fail "sqenvcom must clear SQ_CLASSPATH for lite"

grep -q 'hbcheck 4 30' "$sqstart" || fail "default sqstart path must still run hbcheck"
grep -q 'cleanZKNodes' "$sqstart" || fail "default sqstart path must still clean ZooKeeper nodes"

grep -q 'librocksdb-dev' "$deps" || fail "apt lite dependencies must include RocksDB headers"
grep -q 'rocksdb-devel' "$deps" || fail "rpm lite dependencies must include RocksDB headers"
grep -q 'ROCKSDB_LIB' "$makerules" || fail "SQL makerules must define RocksDB link flags"
grep -q -- '-lrocksdb' "$makerules" || fail "lite SQL link flags must include librocksdb"
grep -q 'litestore' "$executor_makefile" || fail "executor build must include the litestore source path"
grep -q 'LiteRocksDBStore.cpp' "$executor_makefile" || fail "executor build must compile the RocksDB Lite store"
grep -q 'LiteRowCodec.cpp' "$executor_makefile" || fail "executor build must compile the lite row codec"
grep -q 'LiteRocksDBStore.cpp' "$generator_makefile" ||
  fail "generator build must compile lite store metadata access"
grep -q 'LiteRowCodec.cpp' "$generator_makefile" ||
  fail "generator build must compile lite primary key codec"
grep -q 'LiteUnsupportedHbaseTcb' "$storage_stubs" ||
  fail "lite HBase access build must create an executor TCB instead of returning NULL"
grep -q 'LiteRocksdbScanTcb' "$storage_stubs" ||
  fail "lite SELECT must use an executor scan TCB"
grep -q 'LiteHbaseInsertTcb' "$storage_stubs" ||
  fail "lite INSERT must use an executor insert TCB"
grep -q 'listOfGetRows' "$storage_stubs" ||
  fail "lite executor scan must consume optimized get-row requests"
grep -q 'LiteTxn::getRowByKey' "$litestore_source" ||
  fail "lite get-row access must go through the transaction facade"
grep -q 'beginStatement(this, transactionId)' "$litestore_source" ||
  fail "explicit Lite transactions must own a persistent read context"
grep -q 'readOwner = this' "$litestore_source" ||
  fail "transaction scans must resolve through the transaction snapshot"
grep -q 'commitPendingRows' "$litestore_source" ||
  fail "transaction commit must publish each table through one storage batch"
grep -q 'rocksdb_write(db, writeOptions, batch' "$litestore_source" ||
  fail "lite table publication must use a RocksDB write batch"
grep -q 'failed primary-key commit partially published' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must cover atomic primary-key commit failure"
grep -q 'failed UNIQUE commit partially published' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must cover atomic UNIQUE commit failure"
grep -q 'LiteTxn txn' "$storage_stubs" ||
  fail "lite executor scan/insert TCBs must use the transaction facade"
grep -q 'getExecutionCount' "$storage_stubs" ||
  fail "lite executor scans must bind to the current statement execution"
grep -q 'LiteTxnManager::beginStatement' "$executor_root" ||
  fail "executor root must begin the lite statement snapshot context"
grep -q 'LiteTxnManager::endStatement' "$executor_root" ||
  fail "executor root must release the lite statement snapshot context"
if grep -q 'allocateRowId' "$litestore_header" ||
   grep -q 'LiteRocksDBStore::allocateRowId' "$litestore_source" ||
   grep -q 'LiteRocksDBStore::putRow' "$litestore_source"; then
  fail "lite store must not expose legacy row-id allocation or direct row put APIs"
fi
grep -q 'TRAF_LITE_TRACE_SCAN' "$storage_stubs" ||
  fail "lite executor scan must expose test-only scan path tracing"
grep -q 'LiteProjectBinaryRow' "$storage_stubs" ||
  fail "lite executor scan must project from binary persisted rows"
grep -q 'LiteNormalizeBinaryRow' "$storage_stubs" ||
  fail "lite executor insert must persist normalized executor binary aligned rows"
grep -q 'ExpTupleDesc::getVarOffset' "$litestore_codec_source" ||
  fail "lite insert normalization must resolve indirect aligned VARCHAR offsets through the VOA"
grep -q 'uniqueKeyColumns' "$litestore_header" ||
  fail "lite table metadata must preserve UNIQUE key columns"
grep -q 'LiteBuildUniqueKey' "$litestore_source" ||
  fail "lite insert path must build UNIQUE secondary keys"
grep -q 'beginForExecutor' "$litestore_header" ||
  fail "lite transaction manager must expose executor-bound begin"
grep -q 'commitForExecutor' "$litestore_header" ||
  fail "lite transaction manager must expose executor-bound commit"
grep -q 'rollbackForExecutor' "$litestore_header" ||
  fail "lite transaction manager must expose executor-bound rollback"
grep -q 'currentExecutorTxnId' "$litestore_header" ||
  fail "lite transaction manager must expose current executor transaction id"
grep -q 'LiteTxnManager::beginForExecutor' "$ex_transaction" ||
  fail "transaction TCB must begin lite transactions through executor-bound facade"
grep -q 'LiteTxnManager::commitForExecutor' "$ex_transaction" ||
  fail "transaction TCB must commit lite transactions through executor-bound facade"
grep -q 'LiteTxnManager::rollbackForExecutor' "$ex_transaction" ||
  fail "transaction TCB must rollback lite transactions through executor-bound facade"
grep -q 'LiteTxnContext \*liteTxnContext_' "$transaction_header" ||
  fail "each CLI transaction coordinator must own a lite transaction context"
grep -q 'getLiteTxnContext' "$context_header" ||
  fail "CLI context must expose its lite transaction context"
grep -q 'LiteTxnManager::createContext' "$ex_transaction" ||
  fail "ExTransaction construction must create lite transaction state"
grep -q 'resetLiteTransaction' "$context_source" ||
  fail "CLI session reset/teardown must discard lite transaction state"
grep -q 'LiteTxnManager::destroyContext' "$ex_transaction" ||
  fail "ExTransaction destruction must destroy lite transaction state"
if grep -q 'LiteTxnState' "$litestore_header" "$litestore_source"; then
  fail "lite mutable transaction state must not remain process-global"
fi
grep -q 'getLiteTxnContext' "$storage_stubs" ||
  fail "lite scan/DML TCBs must resolve transaction state from the CLI context"
grep -q 'getLiteTxnContext' "$executor_root" ||
  fail "executor root statement snapshots must resolve the CLI transaction context"
grep -q 'beginLiteTransaction' "$ex_transaction" ||
  fail "transaction TCB must use the ExTransaction-owned lite state"
if grep -q 'LiteDecodeFields' "$storage_stubs"; then
  fail "lite executor scan must not decode SQLCI text field rows"
fi
grep -q 'LiteSqlTable.cpp' "$sqlcilib_makefile" ||
  fail "sqlcilib build must compile the lite SQL table handler"
if grep -q 'processInsert' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept INSERT after executor insert migration"
fi
if grep -q 'LiteEncodeBinaryRow' "$local_sql_handler"; then
  fail "SQLCI handler must not encode INSERT rows after executor insert migration"
fi
grep -q 'LiteRocksDBStore.cpp' "$sqlcomp_makefile" ||
  fail "sqlcomp build must compile the RocksDB Lite store"
grep -q 'litestore' "$sqlcomp_makefile" ||
  fail "sqlcomp build must include the litestore source path"
grep -q 'LiteSqlTable_process' "$sqlcmd_source" ||
  fail "DML processing must route lite table statements before CLI prepare"
[[ -f "$local_sql_handler" ]] || fail "missing lite SQL table handler: $local_sql_handler"
[[ -f "$local_sqlci_smoke" ]] || fail "missing lite RocksDB SQLCI smoke test: $local_sqlci_smoke"
[[ -x "$local_store_concurrency" ]] ||
  fail "missing executable lite store concurrency test: $local_store_concurrency"
grep -q 'same-primary-key' "$local_store_concurrency" ||
  fail "lite concurrency test must cover simultaneous primary-key conflicts"
grep -q 'same-unique-key' "$local_store_concurrency" ||
  fail "lite concurrency test must cover simultaneous UNIQUE-key conflicts"
grep -q 'unique conflicts advanced keyless row ids' "$local_store_concurrency" ||
  fail "lite concurrency test must guard keyless row-id metadata after conflicts"
[[ -x "$local_store_process_boundary" ]] ||
  fail "missing executable lite store process-boundary test: $local_store_process_boundary"
[[ -x "$local_statement_snapshot" ]] ||
  fail "missing executable lite statement snapshot test: $local_statement_snapshot"
[[ -x "$local_transaction_snapshot" ]] ||
  fail "missing executable lite transaction snapshot test: $local_transaction_snapshot"
[[ -x "$local_session_transactions" ]] ||
  fail "missing executable lite session transaction test: $local_session_transactions"
[[ -x "$local_context_transactions" ]] ||
  fail "missing executable lite ContextCli transaction test: $local_context_transactions"
[[ -x "$local_server_test" ]] ||
  fail "missing executable Trafodion Lite standalone server test: $local_server_test"
[[ -x "$local_t4jdbc_test" ]] ||
  fail "missing executable Trafodion Lite T4 JDBC test: $local_t4jdbc_test"
[[ -f "$local_t4jdbc_source" ]] ||
  fail "missing Trafodion Lite T4 JDBC source: $local_t4jdbc_source"
[[ -f "$local_row_codec_test_stubs" ]] ||
  fail "missing lite row-codec standalone test stubs: $local_row_codec_test_stubs"
grep -q 'cross-session transaction ID was accepted' "$local_session_transactions" ||
  fail "session transaction test must reject cross-session transaction control"
grep -q 'same-key commit conflict was not deterministic' "$local_session_transactions" ||
  fail "session transaction test must cover deterministic same-key conflicts"
grep -q 'session reset cleanup failed' "$local_session_transactions" ||
  fail "session transaction test must cover reset cleanup"
grep -q 'session destroy cleanup failed' "$local_session_transactions" ||
  fail "session transaction test must cover destroy cleanup"
grep -q 'CREATECONTEXT' "$local_context_transactions" ||
  fail "ContextCli transaction test must create independent CLI contexts"
grep -q 'RESETCONTEXT 2001' "$local_context_transactions" ||
  fail "ContextCli transaction test must cover session reset cleanup"
grep -q 'DELETECONTEXT 2001' "$local_context_transactions" ||
  fail "ContextCli transaction test must cover session deletion cleanup"
[[ -x "$local_regress" ]] ||
  fail "missing executable lite regress runner: $local_regress"
[[ -x "$local_regress_dir/FILTER" ]] ||
  fail "missing executable lite regress output filter"
[[ -f "$local_legacy_manifest" ]] ||
  fail "missing lite legacy regress manifest"
[[ -f "$local_legacy_extra_manifest" ]] ||
  fail "missing Hive/QAT standard regress manifest"
[[ -f "$local_newregr_manifest" ]] ||
  fail "missing separate newregr inventory"
[[ -f "$local_m2_m6_reprobe" ]] ||
  fail "missing M2-M6 legacy re-probe snapshot"
[[ -x "$local_legacy_regress" ]] ||
  fail "missing executable lite legacy regress adapter"
[[ -x "$local_legacy_audit" ]] ||
  fail "missing executable lite legacy regress audit"
[[ -x "$local_upstream_audit" ]] ||
  fail "missing executable complete upstream regress audit"
[[ -x "$local_legacy_m10" ]] ||
  fail "missing executable lite M10 convergence gate"
[[ -x "$local_metadata" ]] ||
  fail "missing executable lite metadata SQL check"
"$local_legacy_audit" --check >/dev/null ||
  fail "lite legacy regress manifest audit failed"
"$local_upstream_audit" --check >/dev/null ||
  fail "complete upstream regress inventory audit failed"
[[ $(awk -F '\t' '!/^#/ && $1 != "suite" && $5 == "pass" { count++ }
       END { print count + 0 }' "$local_m2_m6_reprobe") -eq 5 ]] ||
  fail "M2-M6 re-probe snapshot must retain five exact promotions"
grep -Fq 'native_expected=${#native_tests[@]}' "$local_legacy_m10" ||
  fail "lite M10 convergence gate must derive the native case count"
grep -Fq 'Summary: $native_expected passed, 0 failed' "$local_legacy_m10" ||
  fail "lite M10 convergence gate must check the complete native lane"
grep -q 'TRAF_LITE_STORE_DIR' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must isolate its RocksDB store"
grep -q 'lowercase_name=${copied_name,,}' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must support lowercase self-OBEY names"
grep -q 'append_section' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must materialize selected section bodies"
grep -q -- "-iname 'LOG\*'" "$local_legacy_regress" ||
  fail "lite legacy regress adapter must find case-insensitive LOG names"
grep -q 'unsafe directive detected before SQLCI start' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must reject unsafe helpers"
grep -q '\[A-Za-z_\]\[A-Za-z0-9_\]\*' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must reject macro-based external OBEY"
grep -Fq 'cleanup[[:space:]]+obsolete' "$local_legacy_regress" ||
  fail "lite legacy regress adapter must reject crashing volatile cleanup"
shopt -s nullglob
local_regress_tests=("$local_regress_dir"/TEST[0-9][0-9][0-9])
(( ${#local_regress_tests[@]} > 0 )) ||
  fail "lite regress lane must contain TESTnnn cases"
for test_file in "${local_regress_tests[@]}"; do
  test_number=${test_file##*TEST}
  [[ -f "$local_regress_dir/EXPECTED$test_number" ]] ||
    fail "lite regress lane is missing EXPECTED$test_number"
done
grep -q 'TRAF_LITE_STORE_DIR' "$local_regress" ||
  fail "lite regress cases must use isolated RocksDB stores"
grep -q 'MXID' "$local_regress_dir/FILTER" ||
  fail "lite regress output must normalize dynamic diagnostic ids"
grep -q 'lite-regress' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the lite regress lane"
grep -q '^lite-m11a:' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the M11A session context gate"
grep -q '^lite-m11b:' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the M11B server gate"
grep -q '^lite-m11c:' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the M11C client gate"
grep -q '^lite-m11:' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the complete M11 gate"
grep -q 'lite-regress-inventory' "$repo_root/Makefile" ||
  fail "top-level Makefile must expose the complete regress inventory audit"
[[ -f "$litestore_header" ]] || fail "missing Lite store header: $litestore_header"
[[ -f "$litestore_source" ]] || fail "missing Lite store source: $litestore_source"
grep -q 'LiteBuildPrimaryKeyFromTextFields' "$litestore_codec_header" ||
  fail "lite row codec must expose compiler primary-key literal encoding"
grep -q 'LiteBuildPrimaryKeyFromTextFields' "$litestore_codec_source" ||
  fail "lite row codec must build primary keys from compiler literals"
grep -q 'TRAF_LITE_STORE_DIR' "$litestore_source" || fail "Lite store must support TRAF_LITE_STORE_DIR override"
grep -q 'already open by another' "$litestore_source" ||
  fail "Lite store must explain cross-process RocksDB lock failures"
grep -q 'litestore/rocksdb' "$litestore_source" || fail "Lite store must default under TRAF_VAR/litestore/rocksdb"
if grep -q 'processCreate' "$local_sql_handler"; then
  fail "SQLCI handler must not parse CREATE TABLE after compiler DDL migration"
fi
if grep -q 'processDrop' "$local_sql_handler"; then
  fail "SQLCI handler must not parse DROP TABLE after compiler DDL migration"
fi
if grep -q 'processSelect' "$local_sql_handler"; then
  fail "SQLCI handler must not intercept SELECT after executor scan migration"
fi
grep -q 'executeSeabaseDDL(liteDDLExpr, boundLocalDDL' "$cmp_stmt" ||
  fail "embedded compiler DDL path must dispatch Lite table DDL directly"
grep -q 'liteStorageDDL' "$cmp_ddl_common" ||
  fail "Seabase DDL common path must identify Lite table DDL"
grep -q '(NOT liteStorageDDL) && sendAllControlsAndFlags()' "$cmp_ddl_common" ||
  fail "Lite table DDL must skip sendAllControlsAndFlags"
grep -q 'startXn = FALSE' "$cmp_ddl_common" ||
  fail "Lite table DDL must disable DDL transaction start"
grep -q 'liteCreateTable' "$cmp_ddl_table" ||
  fail "CREATE TABLE must route to the Lite RocksDB catalog from sqlcomp"
grep -q 'liteDropTable' "$cmp_ddl_table" ||
  fail "DROP TABLE must route to the Lite RocksDB catalog from sqlcomp"
[[ $(grep -c 'ComUser::getCurrentUsername()' "$cmp_ddl_table") -eq 2 ]] ||
  fail "lite table ownership must come from the authoritative ContextCli identity"
grep -q 'ComUser::getCurrentUsername()' "$cmp_ddl_common" ||
  fail "lite transition-table ownership must come from ContextCli"
grep -q 'ComUser::getCurrentUsername()' "$cmp_ddl_view" ||
  fail "lite view ownership must come from ContextCli"
if rg -q 'getDatabaseUserName\(\).*data\(\)' \
    "$cmp_ddl_table" "$cmp_ddl_common" "$cmp_ddl_view"; then
  fail "lite DDL ownership must not depend on the compiler session mirror"
fi
grep -q 'xnNeeded() = FALSE' "$gen_precode" ||
  fail "generator pre-code must mark lite CREATE/DROP as no transaction"
grep -q 'liteRewritePrimaryGetRows' "$gen_precode" ||
  fail "generator pre-code must map primary-key equality to lite get-row keys"
grep -Fq 'table.primaryKeyColumns[i]' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "lite NATable must expose real primary-key column ordinals"
grep -Fq 'table.uniqueKeyColumns' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "lite NATable must expose UNIQUE key metadata"
grep -Fq 'setUnique(TRUE)' "$repo_root/core/sql/optimizer/NATable.cpp" ||
  fail "lite NATable UNIQUE metadata must create unique access paths"
grep -q 'LITE_SCAN_GET_ROW' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must verify primary-key equality uses get-row scan"
grep -q 'LITE_SCAN_FULL' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must verify non-key predicates fall back to full scan"
grep -q 'autocommit multi-row VALUES partially published' "$local_sqlci_smoke" ||
  fail "SQLCI smoke must cover atomic autocommit multi-row VALUES failure"
grep -q 'ddl_tdb->setHbaseDDL(TRUE)' "$gen_relmisc" ||
  fail "generator must route lite CREATE/DROP through PROCESSDDL"
grep -q 'ddl_tdb->setHbaseDDLNoUserXn(FALSE)' "$gen_relmisc" ||
  fail "lite CREATE/DROP must retain its bounded user-transaction semantics"
grep -q 'switchToCmpContext' "$ex_ddl" ||
  fail "executor DDL path must initialize embedded arkcmp for lite"
grep -q 'compatibility contract explicitly permits CTAS' "$ex_ctas" ||
  fail "lite CTAS must remain available inside its bounded user transaction"

[[ -f "$server_source" ]] || fail "missing Trafodion Lite server source"
[[ -f "$server_makefile" ]] || fail "missing Trafodion Lite server makefile"
grep -q 'trafodion_lite_server' "$makerules" ||
  fail "lite build must include the Trafodion Lite server"
grep -q 'SQL_EXEC_CreateContext' "$server_source" ||
  fail "Trafodion Lite server must create one CLI context per connection"
grep -q 'LiteRocksDBStore storeLease_' "$server_source" ||
  fail "Trafodion Lite server must hold the Lite store for its process lifetime"
grep -q 'refusing to replace a non-socket Unix path' "$server_source" ||
  fail "Trafodion Lite server must preserve unrelated Unix socket path entries"
grep -q 'unixSocketInode_' "$server_source" ||
  fail "Trafodion Lite server must unlink only the exact Unix socket it created"
grep -q 'clientThreadDone_' "$server_source" ||
  fail "Trafodion Lite server must reap completed connection threads"
grep -q 'kT4GetObjectRef' "$server_source" ||
  fail "Trafodion Lite server must implement the T4 association handshake"
grep -q 'kT4SqlConnect' "$server_source" ||
  fail "Trafodion Lite server must implement the T4 SQL dialogue handshake"
grep -q 'SQL_EXEC_Cancel' "$server_source" ||
  fail "Trafodion Lite server must expose CLI statement cancellation"
grep -q 'LiteSqlTable_isUtilityStatement' "$server_source" ||
  fail "Trafodion Lite extended-query Describe must not execute SQLCI utilities"
grep -q 'OpenTemporary' "$server_source" ||
  fail "Trafodion Lite diagnostics must not leave named files after a crash"
grep -q 'kT4Fetch' "$server_source" ||
  fail "Trafodion Lite T4 server must support result fetching"
grep -q 'kT4Cancel' "$server_source" ||
  fail "Trafodion Lite T4 server must expose the T4 cancel operation"
grep -q 'testCancellationWithPeer' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must cancel a statement without damaging a peer"
grep -q 'testOverlappingTransactions' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must exercise overlapping transaction sessions"
grep -q 'testDisconnectRollback' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must verify disconnect rollback"
grep -q 'testResultTypes' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must verify JDBC result metadata and typed values"
grep -q 'testMetadata' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must verify supported DatabaseMetaData calls"
grep -q 'replacement-must-survive' "$local_server_test" ||
  fail "server gate must preserve a Unix socket pathname replacement"
grep -q 'verifyRestart' "$local_t4jdbc_source" ||
  fail "T4 JDBC gate must verify committed data after restart"

sq_classpath=$(
  cd "$repo_root/core/sqf"
  TRAF_LITE=1 TRAF_CLUSTER_ID=1 TRAF_INSTANCE_ID=1 bash -lc \
    'source ./sqenv.sh >/dev/null 2>/dev/null; printf "%s" "$SQ_CLASSPATH"'
)

[[ -z "$sq_classpath" ]] || fail "lite SQ_CLASSPATH must be empty, got: $sq_classpath"

echo "lite runtime script checks passed"
