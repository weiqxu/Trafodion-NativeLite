#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cxx=${CXX:-/usr/bin/g++}
[[ -x "$cxx" ]] || command -v "$cxx" >/dev/null 2>&1 ||
  fail "missing C++ compiler: $cxx"

work_dir=$(mktemp -d /tmp/traf-lite-transaction-snapshot.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/lite_transaction_snapshot.cpp"
bin="$work_dir/lite_transaction_snapshot"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LiteRocksDBStore.h"

#include <stdio.h>
#include <string>
#include <vector>

class TxnContextOwner
{
public:
  TxnContextOwner() : context_(LiteTxnManager::createContext()) {}
  ~TxnContextOwner() { LiteTxnManager::destroyContext(context_); }
  LiteTxnContext *get() const { return context_; }

private:
  LiteTxnContext *context_;
};

bool LiteBuildPrimaryKey(const LiteTableDef &table,
                              const std::string &encodedRow,
                              std::string *key,
                              std::string *error)
{
  if (table.primaryKeyColumns.empty())
    {
      if (error)
        *error = "primary-key codec called for keyless table";
      return false;
    }
  *key = "P" + encodedRow;
  if (error)
    error->clear();
  return true;
}

bool LiteBuildUniqueKey(const LiteTableDef &,
                             const std::string &encodedRow,
                             const std::vector<size_t> &keyColumns,
                             size_t keyIndex,
                             std::string *key,
                             bool *hasKey,
                             std::string *error)
{
  if (!keyColumns.empty())
    {
      *key = "U";
      *key += static_cast<char>(keyIndex);
      *key += encodedRow;
    }
  if (hasKey)
    *hasKey = !keyColumns.empty();
  if (error)
    error->clear();
  return true;
}

bool LiteBuildOrderedSecondaryKeyPayload(
    const LiteTableDef &,
    const LiteIndexDef &,
    const std::string &,
    std::string *payload,
    bool *hasKey,
    bool *containsNull,
    std::string *error)
{
  if (payload)
    payload->clear();
  if (hasKey)
    *hasKey = false;
  if (containsNull)
    *containsNull = false;
  if (error)
    *error = "ordered index codec is not used by the transaction probe";
  return false;
}

static std::string rowKey(uint64_t rowId)
{
  std::string key;
  for (int shift = 56; shift >= 0; shift -= 8)
    key += static_cast<char>((rowId >> shift) & 0xff);
  return key;
}

static bool directInsert(LiteRocksDBStore *store,
                         const LiteTableDef &table,
                         const char *value,
                         uint64_t expectedRowId)
{
  uint64_t rowId = 0;
  std::string error;
  if (!store->insertRow(table, value, &rowId, &error))
    {
      fprintf(stderr, "direct insert failed: %s\n", error.c_str());
      return false;
    }
  if (rowId != expectedRowId)
    {
      fprintf(stderr, "expected inserted row id %llu, got %llu\n",
              static_cast<unsigned long long>(expectedRowId),
              static_cast<unsigned long long>(rowId));
      return false;
    }
  return true;
}

static bool scanCount(LiteTxn *txn,
                      const LiteTableDef &table,
                      size_t expected)
{
  std::vector<LiteRow> rows;
  std::string error;
  if (!txn->scanRows(table, &rows, &error))
    {
      fprintf(stderr, "scan failed: %s\n", error.c_str());
      return false;
    }
  if (rows.size() != expected)
    {
      fprintf(stderr, "expected %zu rows, found %zu\n",
              expected, rows.size());
      return false;
    }
  return true;
}

static bool scanValues(LiteTxn *txn,
                       const LiteTableDef &table,
                       size_t expectedCount,
                       const char *required,
                       const char *forbidden)
{
  std::vector<LiteRow> rows;
  std::string error;
  if (!txn->scanRows(table, &rows, &error))
    {
      fprintf(stderr, "scan failed: %s\n", error.c_str());
      return false;
    }

  bool foundRequired = false;
  bool foundForbidden = false;
  for (size_t i = 0; i < rows.size(); i++)
    {
      foundRequired = foundRequired || rows[i].value == required;
      foundForbidden = foundForbidden || rows[i].value == forbidden;
    }
  if (rows.size() != expectedCount || !foundRequired || foundForbidden)
    {
      fprintf(stderr,
              "unexpected scan: count=%zu required=%s found=%d "
              "forbidden=%s found=%d\n",
              rows.size(), required, foundRequired ? 1 : 0,
              forbidden, foundForbidden ? 1 : 0);
      return false;
    }
  return true;
}

static bool updateValue(LiteTxn *txn,
                        const LiteTableDef &table,
                        const char *before,
                        const char *after)
{
  std::vector<LiteRow> rows;
  std::string error;
  if (!txn->scanRows(table, &rows, &error))
    {
      fprintf(stderr, "update source scan failed: %s\n", error.c_str());
      return false;
    }
  for (size_t i = 0; i < rows.size(); i++)
    if (rows[i].value == before)
      {
        LiteRowMutation mutation;
        mutation.before = rows[i];
        mutation.after = after;
        std::vector<LiteRowMutation> mutations(1, mutation);
        if (!txn->updateRows(table, mutations, &error))
          {
            fprintf(stderr, "pending update failed: %s\n", error.c_str());
            return false;
          }
        return true;
      }
  fprintf(stderr, "update source value not found: %s\n", before);
  return false;
}

static bool getFound(LiteTxn *txn,
                     const LiteTableDef &table,
                     uint64_t rowId,
                     bool expected)
{
  LiteRow row;
  bool found = false;
  std::string error;
  if (!txn->getRowByKey(table, rowKey(rowId), &row, &found, &error))
    {
      fprintf(stderr, "get row failed: %s\n", error.c_str());
      return false;
    }
  if (found != expected)
    {
      fprintf(stderr, "row %llu expected found=%d, got %d\n",
              static_cast<unsigned long long>(rowId),
              expected ? 1 : 0, found ? 1 : 0);
      return false;
    }
  return true;
}

static bool finishStatement(LiteTxnContext *txnContext,
                            const void *owner,
                            uint64_t executionId)
{
  LiteTxnManager::endStatement(txnContext, owner, executionId);
  return true;
}

int main()
{
  TxnContextOwner session;
  LiteTxnContext *txnContext = session.get();
  LiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  table.name = "TX_SNAPSHOT_T";
  table.objectUid = 3001;
  table.nextRowId = 1;
  LiteColumnDef column;
  column.name = "A";
  column.type = "VARCHAR(32)";
  column.nullable = false;
  table.columns.push_back(column);

  LiteTableDef secondTable = table;
  secondTable.name = "TX_SNAPSHOT_SECOND_T";
  secondTable.objectUid = 3004;

  LiteRocksDBStore setupStore;
  std::string error;
  if (!setupStore.createTable(table, &error) ||
      !directInsert(&setupStore, table, "before-transaction", 1) ||
      !setupStore.createTable(secondTable, &error) ||
      !directInsert(&setupStore, secondTable, "second-before-transaction", 1))
    return 1;
  setupStore.close();

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "begin transaction failed: %s\n", error.c_str());
      return 1;
    }

  int firstStatement = 0;
  LiteTxnManager::beginStatement(txnContext, &firstStatement, 11);
  LiteRocksDBStore firstScanStore;
  LiteTxn firstScan(&firstScanStore, txnContext, &firstStatement, 11);
  if (!scanCount(&firstScan, table, 1))
    return 1;
  finishStatement(txnContext, &firstStatement, 11);
  firstScanStore.close();

  LiteRocksDBStore committedWriter;
  if (!directInsert(&committedWriter, table, "committed-after-read", 2) ||
      !directInsert(&committedWriter, secondTable,
                    "second-committed-after-begin", 2))
    return 1;
  committedWriter.close();

  int secondStatement = 0;
  LiteTxnManager::beginStatement(txnContext, &secondStatement, 12);
  LiteRocksDBStore repeatedScanStore;
  LiteTxn repeatedScan(&repeatedScanStore, txnContext, &secondStatement, 12);
  if (!scanCount(&repeatedScan, table, 1) ||
      !getFound(&repeatedScan, table, 2, false) ||
      // The second table is first touched after the concurrent commit. It
      // must still use the snapshot captured at transaction begin.
      !scanCount(&repeatedScan, secondTable, 1) ||
      !getFound(&repeatedScan, secondTable, 2, false))
    return 1;
  finishStatement(txnContext, &secondStatement, 12);
  repeatedScanStore.close();

  LiteRocksDBStore pendingWriterStore;
  LiteTxn pendingWriter(&pendingWriterStore, txnContext);
  uint64_t pendingRowId = 0;
  if (!pendingWriter.insertRow(table, "pending-own-write",
                               &pendingRowId, &error) ||
      pendingRowId != 3)
    {
      fprintf(stderr, "pending insert failed: %s\n", error.c_str());
      return 1;
    }
  pendingWriterStore.close();

  int ownWriteStatement = 0;
  LiteTxnManager::beginStatement(txnContext, &ownWriteStatement, 13);
  LiteRocksDBStore ownWriteScanStore;
  LiteTxn ownWriteScan(&ownWriteScanStore, txnContext,
                            &ownWriteStatement, 13);
  if (!scanCount(&ownWriteScan, table, 2) ||
      !getFound(&ownWriteScan, table, pendingRowId, true) ||
      !getFound(&ownWriteScan, table, 2, false))
    return 1;
  LiteOccState occState;
  if (!LiteTxnManager::occState(txnContext, &occState) ||
      occState.startSequence == 0 || occState.fullScans == 0 ||
      occState.pointReads == 0 || occState.missingPointReads == 0 ||
      occState.readRanges == 0 || occState.writeKeys == 0)
    {
      fprintf(stderr,
              "incomplete OCC state: start=%llu ranges=%llu writes=%llu "
              "points=%llu missing=%llu scans=%llu\n",
              static_cast<unsigned long long>(occState.startSequence),
              static_cast<unsigned long long>(occState.readRanges),
              static_cast<unsigned long long>(occState.writeKeys),
              static_cast<unsigned long long>(occState.pointReads),
              static_cast<unsigned long long>(occState.missingPointReads),
              static_cast<unsigned long long>(occState.fullScans));
      return 1;
    }
  finishStatement(txnContext, &ownWriteStatement, 13);
  ownWriteScanStore.close();

  if (!LiteTxnManager::rollback(txnContext, &error))
    {
      fprintf(stderr, "rollback transaction failed: %s\n", error.c_str());
      return 1;
    }

  LiteRocksDBStore afterRollbackStore;
  LiteTxn afterRollback(&afterRollbackStore, txnContext);
  if (!scanCount(&afterRollback, table, 2) ||
      !getFound(&afterRollback, table, 2, true) ||
      !getFound(&afterRollback, table, pendingRowId, false))
    return 1;
  afterRollbackStore.close();

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "second begin transaction failed: %s\n", error.c_str());
      return 1;
    }
  if (!LiteTxnManager::occState(txnContext, &occState) ||
      occState.readRanges != 0 || occState.writeKeys != 0 ||
      occState.pointReads != 0 || occState.fullScans != 0)
    {
      fprintf(stderr, "OCC state leaked across transaction boundary\n");
      return 1;
    }

  int commitReadStatement = 0;
  LiteTxnManager::beginStatement(txnContext, &commitReadStatement, 21);
  LiteRocksDBStore commitReadStore;
  LiteTxn commitRead(&commitReadStore, txnContext,
                          &commitReadStatement, 21);
  if (!scanCount(&commitRead, table, 2))
    return 1;
  finishStatement(txnContext, &commitReadStatement, 21);
  commitReadStore.close();

  if (!directInsert(&committedWriter, table, "committed-before-commit", 3))
    return 1;
  committedWriter.close();

  int commitRepeatStatement = 0;
  LiteTxnManager::beginStatement(txnContext, &commitRepeatStatement, 22);
  LiteRocksDBStore commitRepeatStore;
  LiteTxn commitRepeat(&commitRepeatStore, txnContext,
                            &commitRepeatStatement, 22);
  if (!scanCount(&commitRepeat, table, 2) ||
      !getFound(&commitRepeat, table, 3, false))
    return 1;
  finishStatement(txnContext, &commitRepeatStatement, 22);
  commitRepeatStore.close();

  if (!LiteTxnManager::commit(txnContext, &error))
    {
      fprintf(stderr, "commit transaction failed: %s\n", error.c_str());
      return 1;
    }

  LiteRocksDBStore afterCommitStore;
  LiteTxn afterCommit(&afterCommitStore, txnContext);
  if (!scanCount(&afterCommit, table, 3) ||
      !getFound(&afterCommit, table, 3, true))
    return 1;

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "update rollback begin failed: %s\n", error.c_str());
      return 1;
    }
  if (!updateValue(&afterCommit, table,
                   "before-transaction", "rollback-update") ||
      !scanValues(&afterCommit, table, 3,
                  "rollback-update", "before-transaction"))
    return 1;
  if (!LiteTxnManager::rollback(txnContext, &error) ||
      !scanValues(&afterCommit, table, 3,
                  "before-transaction", "rollback-update"))
    {
      fprintf(stderr, "update rollback failed: %s\n", error.c_str());
      return 1;
    }

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "update commit begin failed: %s\n", error.c_str());
      return 1;
    }
  if (!updateValue(&afterCommit, table,
                   "before-transaction", "pending-update") ||
      !updateValue(&afterCommit, table,
                   "pending-update", "committed-update") ||
      !scanValues(&afterCommit, table, 3,
                  "committed-update", "before-transaction") ||
      !LiteTxnManager::commit(txnContext, &error) ||
      !scanValues(&afterCommit, table, 3,
                  "committed-update", "before-transaction"))
    {
      fprintf(stderr, "update commit failed: %s\n", error.c_str());
      return 1;
    }

  LiteTableDef primaryTable = table;
  primaryTable.name = "TX_ATOMIC_PK_T";
  primaryTable.objectUid = 3002;
  primaryTable.primaryKeyColumns.push_back(0);
  primaryTable.primaryKeyName = "TX_ATOMIC_PK";
  if (!afterCommitStore.createTable(primaryTable, &error) ||
      !directInsert(&afterCommitStore, primaryTable, "duplicate", 1))
    return 1;

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "primary atomic begin failed: %s\n", error.c_str());
      return 1;
    }
  LiteRocksDBStore primaryPendingStore;
  LiteTxn primaryPending(&primaryPendingStore, txnContext);
  uint64_t ignoredRowId = 0;
  if (!primaryPending.insertRow(primaryTable, "first",
                                &ignoredRowId, &error) ||
      !primaryPending.insertRow(primaryTable, "duplicate",
                                &ignoredRowId, &error))
    {
      fprintf(stderr, "primary atomic pending insert failed: %s\n",
              error.c_str());
      return 1;
    }
  primaryPendingStore.close();

  error.clear();
  if (LiteTxnManager::commit(txnContext, &error) ||
      error.find("duplicate lite primary key") == std::string::npos)
    {
      fprintf(stderr, "primary atomic commit unexpectedly succeeded: %s\n",
              error.c_str());
      return 1;
    }
  LiteRocksDBStore primaryVerifyStore;
  LiteTxn primaryVerify(&primaryVerifyStore, txnContext);
  if (!scanValues(&primaryVerify, primaryTable, 1,
                  "duplicate", "first"))
    return 1;
  primaryVerifyStore.close();

  LiteTableDef uniqueTable = table;
  uniqueTable.name = "TX_ATOMIC_UNIQUE_T";
  uniqueTable.objectUid = 3003;
  std::vector<size_t> uniqueColumns;
  uniqueColumns.push_back(0);
  uniqueTable.uniqueKeyColumns.push_back(uniqueColumns);
  uniqueTable.uniqueKeyNames.push_back("TX_ATOMIC_UNIQUE");
  if (!afterCommitStore.createTable(uniqueTable, &error) ||
      !directInsert(&afterCommitStore, uniqueTable, "duplicate", 1))
    return 1;

  if (!LiteTxnManager::begin(txnContext, &error))
    {
      fprintf(stderr, "unique atomic begin failed: %s\n", error.c_str());
      return 1;
    }
  LiteRocksDBStore uniquePendingStore;
  LiteTxn uniquePending(&uniquePendingStore, txnContext);
  if (!uniquePending.insertRow(uniqueTable, "first",
                               &ignoredRowId, &error) ||
      !uniquePending.insertRow(uniqueTable, "duplicate",
                               &ignoredRowId, &error))
    {
      fprintf(stderr, "unique atomic pending insert failed: %s\n",
              error.c_str());
      return 1;
    }
  uniquePendingStore.close();

  error.clear();
  if (LiteTxnManager::commit(txnContext, &error) ||
      error.find("duplicate lite unique key") == std::string::npos)
    {
      fprintf(stderr, "unique atomic commit unexpectedly succeeded: %s\n",
              error.c_str());
      return 1;
    }
  LiteRocksDBStore uniqueVerifyStore;
  LiteTxn uniqueVerify(&uniqueVerifyStore, txnContext);
  if (!scanValues(&uniqueVerify, uniqueTable, 1,
                  "duplicate", "first") ||
      !directInsert(&uniqueVerifyStore, uniqueTable, "after-failure", 2))
    return 1;

  printf("lite transaction snapshot probe passed\n");
  return 0;
}
CPP

rocksdb_include_flags=()
if [[ -d /usr/include/x86_64-linux-gnu ]]; then
  rocksdb_include_flags+=(-I/usr/include/x86_64-linux-gnu)
fi
rocksdb_include_flags+=(-I/usr/include)
rocksdb_library_flags=()
if [[ -d /usr/lib/x86_64-linux-gnu ]]; then
  rocksdb_library_flags+=(-L/usr/lib/x86_64-linux-gnu)
fi
if [[ -d /lib/x86_64-linux-gnu ]]; then
  rocksdb_library_flags+=(-L/lib/x86_64-linux-gnu)
fi

"$cxx" -DTRAF_LITE -std=c++0x \
  -I"$repo_root/core/sql/litestore" \
  "${rocksdb_include_flags[@]}" \
  "$src" "$repo_root/core/sql/litestore/LiteRocksDBStore.cpp" \
  "$repo_root/core/sql/litestore/LiteStorage.cpp" \
  "$repo_root/core/sql/litestore/LiteUnifiedRocksDB.cpp" \
  "$repo_root/scripts/lite-row-codec-test-stubs.cpp" \
  "${rocksdb_library_flags[@]}" \
  -lrocksdb -lpthread -o "$bin"

TRAF_LITE_STORE_DIR="$store_dir" "$bin"
