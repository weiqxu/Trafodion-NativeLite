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

work_dir=$(mktemp -d /tmp/traf-local-lite-transaction-snapshot.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/local_lite_transaction_snapshot.cpp"
bin="$work_dir/local_lite_transaction_snapshot"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LocalLiteRocksDBStore.h"

#include <stdio.h>
#include <string>
#include <vector>

bool LocalLiteBuildPrimaryKey(const LocalLiteTableDef &,
                              const std::string &,
                              std::string *,
                              std::string *error)
{
  if (error)
    *error = "primary-key codec should not be used by this probe";
  return false;
}

bool LocalLiteBuildUniqueKey(const LocalLiteTableDef &,
                             const std::string &,
                             const std::vector<size_t> &,
                             size_t,
                             std::string *,
                             bool *hasKey,
                             std::string *error)
{
  if (hasKey)
    *hasKey = false;
  if (error)
    error->clear();
  return true;
}

static std::string rowKey(uint64_t rowId)
{
  std::string key;
  for (int shift = 56; shift >= 0; shift -= 8)
    key += static_cast<char>((rowId >> shift) & 0xff);
  return key;
}

static bool directInsert(LocalLiteRocksDBStore *store,
                         const LocalLiteTableDef &table,
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

static bool scanCount(LocalLiteTxn *txn,
                      const LocalLiteTableDef &table,
                      size_t expected)
{
  std::vector<LocalLiteRow> rows;
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

static bool getFound(LocalLiteTxn *txn,
                     const LocalLiteTableDef &table,
                     uint64_t rowId,
                     bool expected)
{
  LocalLiteRow row;
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

static bool finishStatement(const void *owner, uint64_t executionId)
{
  LocalLiteTxnManager::endStatement(owner, executionId);
  return true;
}

int main()
{
  LocalLiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  table.name = "TX_SNAPSHOT_T";
  table.objectUid = 3001;
  table.nextRowId = 1;
  LocalLiteColumnDef column;
  column.name = "A";
  column.type = "VARCHAR(32)";
  column.nullable = false;
  table.columns.push_back(column);

  LocalLiteRocksDBStore setupStore;
  std::string error;
  if (!setupStore.createTable(table, &error) ||
      !directInsert(&setupStore, table, "before-transaction", 1))
    return 1;
  setupStore.close();

  if (!LocalLiteTxnManager::begin(&error))
    {
      fprintf(stderr, "begin transaction failed: %s\n", error.c_str());
      return 1;
    }

  int firstStatement = 0;
  LocalLiteTxnManager::beginStatement(&firstStatement, 11);
  LocalLiteRocksDBStore firstScanStore;
  LocalLiteTxn firstScan(&firstScanStore, &firstStatement, 11);
  if (!scanCount(&firstScan, table, 1))
    return 1;
  finishStatement(&firstStatement, 11);
  firstScanStore.close();

  LocalLiteRocksDBStore committedWriter;
  if (!directInsert(&committedWriter, table, "committed-after-read", 2))
    return 1;
  committedWriter.close();

  int secondStatement = 0;
  LocalLiteTxnManager::beginStatement(&secondStatement, 12);
  LocalLiteRocksDBStore repeatedScanStore;
  LocalLiteTxn repeatedScan(&repeatedScanStore, &secondStatement, 12);
  if (!scanCount(&repeatedScan, table, 1) ||
      !getFound(&repeatedScan, table, 2, false))
    return 1;
  finishStatement(&secondStatement, 12);
  repeatedScanStore.close();

  LocalLiteRocksDBStore pendingWriterStore;
  LocalLiteTxn pendingWriter(&pendingWriterStore);
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
  LocalLiteTxnManager::beginStatement(&ownWriteStatement, 13);
  LocalLiteRocksDBStore ownWriteScanStore;
  LocalLiteTxn ownWriteScan(&ownWriteScanStore, &ownWriteStatement, 13);
  if (!scanCount(&ownWriteScan, table, 2) ||
      !getFound(&ownWriteScan, table, pendingRowId, true) ||
      !getFound(&ownWriteScan, table, 2, false))
    return 1;
  finishStatement(&ownWriteStatement, 13);
  ownWriteScanStore.close();

  if (!LocalLiteTxnManager::rollback(&error))
    {
      fprintf(stderr, "rollback transaction failed: %s\n", error.c_str());
      return 1;
    }

  LocalLiteRocksDBStore afterRollbackStore;
  LocalLiteTxn afterRollback(&afterRollbackStore);
  if (!scanCount(&afterRollback, table, 2) ||
      !getFound(&afterRollback, table, 2, true) ||
      !getFound(&afterRollback, table, pendingRowId, false))
    return 1;
  afterRollbackStore.close();

  if (!LocalLiteTxnManager::begin(&error))
    {
      fprintf(stderr, "second begin transaction failed: %s\n", error.c_str());
      return 1;
    }

  int commitReadStatement = 0;
  LocalLiteTxnManager::beginStatement(&commitReadStatement, 21);
  LocalLiteRocksDBStore commitReadStore;
  LocalLiteTxn commitRead(&commitReadStore, &commitReadStatement, 21);
  if (!scanCount(&commitRead, table, 2))
    return 1;
  finishStatement(&commitReadStatement, 21);
  commitReadStore.close();

  if (!directInsert(&committedWriter, table, "committed-before-commit", 3))
    return 1;
  committedWriter.close();

  int commitRepeatStatement = 0;
  LocalLiteTxnManager::beginStatement(&commitRepeatStatement, 22);
  LocalLiteRocksDBStore commitRepeatStore;
  LocalLiteTxn commitRepeat(&commitRepeatStore, &commitRepeatStatement, 22);
  if (!scanCount(&commitRepeat, table, 2) ||
      !getFound(&commitRepeat, table, 3, false))
    return 1;
  finishStatement(&commitRepeatStatement, 22);
  commitRepeatStore.close();

  if (!LocalLiteTxnManager::commit(&error))
    {
      fprintf(stderr, "commit transaction failed: %s\n", error.c_str());
      return 1;
    }

  LocalLiteRocksDBStore afterCommitStore;
  LocalLiteTxn afterCommit(&afterCommitStore);
  if (!scanCount(&afterCommit, table, 3) ||
      !getFound(&afterCommit, table, 3, true))
    return 1;

  printf("local-lite transaction snapshot probe passed\n");
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

"$cxx" -DTRAF_LOCAL_LITE -std=c++0x \
  -I"$repo_root/core/sql/localstore" \
  "${rocksdb_include_flags[@]}" \
  "$src" "$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp" \
  "${rocksdb_library_flags[@]}" \
  -lrocksdb -lpthread -o "$bin"

TRAF_LOCAL_STORE_DIR="$store_dir" "$bin"
