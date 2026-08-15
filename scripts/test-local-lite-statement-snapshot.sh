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

work_dir=$(mktemp -d /tmp/traf-local-lite-statement-snapshot.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/local_lite_statement_snapshot.cpp"
bin="$work_dir/local_lite_statement_snapshot"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LocalLiteRocksDBStore.h"

#include <stdio.h>
#include <string>
#include <vector>

class TxnContextOwner
{
public:
  TxnContextOwner() : context_(LocalLiteTxnManager::createContext()) {}
  ~TxnContextOwner() { LocalLiteTxnManager::destroyContext(context_); }
  LocalLiteTxnContext *get() const { return context_; }

private:
  LocalLiteTxnContext *context_;
};

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

bool LocalLiteBuildOrderedSecondaryKeyPayload(
    const LocalLiteTableDef &,
    const LocalLiteIndexDef &,
    const std::string &,
    std::string *,
    bool *hasKey,
    bool *containsNull,
    std::string *error)
{
  if (hasKey)
    *hasKey = false;
  if (containsNull)
    *containsNull = false;
  if (error)
    *error = "ordered index codec is not used by the statement probe";
  return false;
}

static std::string rowKey(uint64_t rowId)
{
  std::string key;
  for (int shift = 56; shift >= 0; shift -= 8)
    key += static_cast<char>((rowId >> shift) & 0xff);
  return key;
}

static bool insert(LocalLiteTxn *txn,
                   const LocalLiteTableDef &table,
                   const char *value,
                   uint64_t *rowId)
{
  std::string error;
  if (!txn->insertRow(table, value, rowId, &error))
    {
      fprintf(stderr, "insert failed: %s\n", error.c_str());
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

int main()
{
  TxnContextOwner session;
  LocalLiteTxnContext *txnContext = session.get();
  LocalLiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  table.name = "SNAPSHOT_T";
  table.objectUid = 2001;
  table.nextRowId = 1;
  LocalLiteColumnDef column;
  column.name = "A";
  column.type = "VARCHAR(32)";
  column.nullable = false;
  table.columns.push_back(column);

  LocalLiteRocksDBStore setupStore;
  std::string error;
  if (!setupStore.createTable(table, &error))
    {
      fprintf(stderr, "create table failed: %s\n", error.c_str());
      return 1;
    }

  LocalLiteTxn setupTxn(&setupStore, txnContext);
  uint64_t firstRowId = 0;
  if (!insert(&setupTxn, table, "before-snapshot", &firstRowId) ||
      firstRowId != 1)
    return 1;

  int statementOwner = 0;
  LocalLiteTxnManager::beginStatement(txnContext, &statementOwner, 7);

  LocalLiteRocksDBStore firstScanStore;
  LocalLiteTxn firstScan(&firstScanStore, txnContext, &statementOwner, 7);
  if (!scanCount(&firstScan, table, 1))
    return 1;

  LocalLiteRocksDBStore writerStore;
  LocalLiteTxn writer(&writerStore, txnContext);
  uint64_t secondRowId = 0;
  if (!insert(&writer, table, "after-snapshot", &secondRowId) ||
      secondRowId != 2)
    return 1;

  LocalLiteRocksDBStore repeatedScanStore;
  LocalLiteTxn repeatedScan(&repeatedScanStore, txnContext, &statementOwner, 7);
  if (!scanCount(&repeatedScan, table, 1) ||
      !getFound(&repeatedScan, table, secondRowId, false))
    return 1;

  LocalLiteTxnManager::endStatement(txnContext, &statementOwner, 7);
  LocalLiteTxnManager::beginStatement(txnContext, &statementOwner, 8);

  LocalLiteRocksDBStore nextExecutionStore;
  LocalLiteTxn nextExecution(&nextExecutionStore, txnContext, &statementOwner, 8);
  if (!scanCount(&nextExecution, table, 2) ||
      !getFound(&nextExecution, table, secondRowId, true))
    return 1;

  LocalLiteTxnManager::endStatement(txnContext, &statementOwner, 8);
  printf("local-lite statement snapshot probe passed\n");
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
  "$repo_root/core/sql/localstore/LocalLiteStorage.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteUnifiedRocksDB.cpp" \
  "$repo_root/scripts/local-lite-row-codec-test-stubs.cpp" \
  "${rocksdb_library_flags[@]}" \
  -lrocksdb -lpthread -o "$bin"

TRAF_LOCAL_STORE_DIR="$store_dir" "$bin"
