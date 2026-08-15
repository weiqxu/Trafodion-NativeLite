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

work_dir=$(mktemp -d /tmp/traf-local-lite-session-transactions.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/local_lite_session_transactions.cpp"
bin="$work_dir/local_lite_session_transactions"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LocalLiteRocksDBStore.h"

#include <stdio.h>
#include <string>
#include <vector>

bool LocalLiteBuildPrimaryKey(const LocalLiteTableDef &table,
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
    *error = "ordered index codec is not used by the session probe";
  return false;
}

static bool scanValues(LocalLiteTxn *txn,
                       const LocalLiteTableDef &table,
                       size_t expectedCount,
                       const char *required,
                       const char *forbidden)
{
  std::vector<LocalLiteRow> rows;
  std::string error;
  if (!txn->scanRows(table, &rows, &error))
    {
      fprintf(stderr, "scan failed: %s\n", error.c_str());
      return false;
    }

  bool foundRequired = required == NULL;
  bool foundForbidden = false;
  for (size_t i = 0; i < rows.size(); i++)
    {
      if (required && rows[i].value == required)
        foundRequired = true;
      if (forbidden && rows[i].value == forbidden)
        foundForbidden = true;
    }
  if (rows.size() != expectedCount || !foundRequired || foundForbidden)
    {
      fprintf(stderr,
              "unexpected scan: count=%zu required=%s found=%d "
              "forbidden=%s found=%d\n",
              rows.size(), required ? required : "<none>",
              foundRequired ? 1 : 0, forbidden ? forbidden : "<none>",
              foundForbidden ? 1 : 0);
      return false;
    }
  return true;
}

static bool insert(LocalLiteTxn *txn,
                   const LocalLiteTableDef &table,
                   const char *value)
{
  uint64_t rowId = 0;
  std::string error;
  if (!txn->insertRow(table, value, &rowId, &error))
    {
      fprintf(stderr, "insert %s failed: %s\n", value, error.c_str());
      return false;
    }
  return true;
}

int main()
{
  LocalLiteTxnContext *sessionA = LocalLiteTxnManager::createContext();
  LocalLiteTxnContext *sessionB = LocalLiteTxnManager::createContext();
  LocalLiteTxnContext *observer = LocalLiteTxnManager::createContext();
  if (!sessionA || !sessionB || !observer)
    {
      fprintf(stderr, "create session transaction context failed\n");
      return 1;
    }

  LocalLiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  table.name = "SESSION_TX_T";
  table.objectUid = 4001;
  table.nextRowId = 1;
  LocalLiteColumnDef column;
  column.name = "A";
  column.type = "VARCHAR(64)";
  column.nullable = false;
  table.columns.push_back(column);
  table.primaryKeyColumns.push_back(0);
  table.primaryKeyName = "SESSION_TX_PK";

  std::string error;
  LocalLiteRocksDBStore setupStore;
  if (!setupStore.createTable(table, &error))
    {
      fprintf(stderr, "create table failed: %s\n", error.c_str());
      return 1;
    }

  // Executor transaction IDs are checked inside their owning session only.
  if (!LocalLiteTxnManager::beginForExecutor(sessionA, 101, &error) ||
      !LocalLiteTxnManager::beginForExecutor(sessionB, 202, &error) ||
      LocalLiteTxnManager::currentExecutorTxnId(sessionA) != 101 ||
      LocalLiteTxnManager::currentExecutorTxnId(sessionB) != 202)
    {
      fprintf(stderr, "independent executor transaction begin failed: %s\n",
              error.c_str());
      return 1;
    }
  error.clear();
  if (LocalLiteTxnManager::rollbackForExecutor(sessionA, 202, &error) ||
      error.find("transaction context mismatch") == std::string::npos ||
      !LocalLiteTxnManager::active(sessionA) ||
      !LocalLiteTxnManager::active(sessionB))
    {
      fprintf(stderr, "cross-session transaction ID was accepted: %s\n",
              error.c_str());
      return 1;
    }
  if (!LocalLiteTxnManager::rollbackForExecutor(sessionA, 101, &error) ||
      !LocalLiteTxnManager::rollbackForExecutor(sessionB, 202, &error))
    {
      fprintf(stderr, "independent executor rollback failed: %s\n",
              error.c_str());
      return 1;
    }

  LocalLiteRocksDBStore storeA;
  LocalLiteRocksDBStore storeB;
  LocalLiteRocksDBStore observerStore;
  LocalLiteTxn txnA(&storeA, sessionA);
  LocalLiteTxn txnB(&storeB, sessionB);
  LocalLiteTxn observerTxn(&observerStore, observer);

  // Both sessions may stage the same key. The shared store serializes commit,
  // so the first committer wins and the second receives a deterministic error.
  if (!LocalLiteTxnManager::begin(sessionA, &error) ||
      !LocalLiteTxnManager::begin(sessionB, &error) ||
      !insert(&txnA, table, "contended") ||
      !insert(&txnB, table, "contended") ||
      !scanValues(&txnA, table, 1, "contended", NULL) ||
      !scanValues(&txnB, table, 1, "contended", NULL) ||
      !scanValues(&observerTxn, table, 0, NULL, "contended"))
    return 1;
  if (!LocalLiteTxnManager::commit(sessionA, &error))
    {
      fprintf(stderr, "first session commit failed: %s\n", error.c_str());
      return 1;
    }
  error.clear();
  if (LocalLiteTxnManager::commit(sessionB, &error) ||
      error.find("duplicate local-lite primary key") == std::string::npos ||
      !scanValues(&observerTxn, table, 1, "contended", NULL))
    {
      fprintf(stderr, "same-key commit conflict was not deterministic: %s\n",
              error.c_str());
      return 1;
    }

  // Commit and rollback remain independent across two active sessions.
  if (!LocalLiteTxnManager::begin(sessionA, &error) ||
      !LocalLiteTxnManager::begin(sessionB, &error) ||
      !insert(&txnA, table, "rollback-a") ||
      !insert(&txnB, table, "commit-b") ||
      !LocalLiteTxnManager::rollback(sessionA, &error) ||
      !LocalLiteTxnManager::commit(sessionB, &error) ||
      !scanValues(&observerTxn, table, 2, "commit-b", "rollback-a"))
    {
      fprintf(stderr, "independent commit/rollback failed: %s\n", error.c_str());
      return 1;
    }

  // Reset releases the session's transaction and statement snapshots without
  // changing the other sessions or making the context unusable.
  int resetStatement = 0;
  if (!LocalLiteTxnManager::begin(sessionB, &error) ||
      !insert(&txnB, table, "discard-on-reset"))
    return 1;
  LocalLiteTxnManager::beginStatement(sessionB, &resetStatement, 31);
  LocalLiteTxn resetScan(&storeB, sessionB, &resetStatement, 31);
  if (!scanValues(&resetScan, table, 3, "discard-on-reset", NULL))
    return 1;
  LocalLiteTxnManager::resetContext(sessionB);
  if (LocalLiteTxnManager::active(sessionB) ||
      !scanValues(&observerTxn, table, 2, "commit-b", "discard-on-reset") ||
      !LocalLiteTxnManager::begin(sessionB, &error) ||
      !insert(&txnB, table, "after-reset") ||
      !LocalLiteTxnManager::commit(sessionB, &error))
    {
      fprintf(stderr, "session reset cleanup failed: %s\n", error.c_str());
      return 1;
    }

  // Destroying a session is an implicit rollback and releases any retained
  // statement snapshot. A replacement session can immediately use the key.
  int destroyStatement = 0;
  if (!LocalLiteTxnManager::begin(sessionA, &error) ||
      !insert(&txnA, table, "discard-on-destroy"))
    return 1;
  LocalLiteTxnManager::beginStatement(sessionA, &destroyStatement, 41);
  LocalLiteTxn destroyScan(&storeA, sessionA, &destroyStatement, 41);
  if (!scanValues(&destroyScan, table, 4, "discard-on-destroy", NULL))
    return 1;
  LocalLiteTxnManager::destroyContext(sessionA);
  sessionA = NULL;

  LocalLiteTxnContext *replacement = LocalLiteTxnManager::createContext();
  LocalLiteRocksDBStore replacementStore;
  LocalLiteTxn replacementTxn(&replacementStore, replacement);
  if (!scanValues(&observerTxn, table, 3, "after-reset", "discard-on-destroy") ||
      !LocalLiteTxnManager::begin(replacement, &error) ||
      !insert(&replacementTxn, table, "discard-on-destroy") ||
      !LocalLiteTxnManager::commit(replacement, &error) ||
      !scanValues(&observerTxn, table, 4, "discard-on-destroy", "rollback-a"))
    {
      fprintf(stderr, "session destroy cleanup failed: %s\n", error.c_str());
      return 1;
    }

  LocalLiteTxnManager::destroyContext(replacement);
  LocalLiteTxnManager::destroyContext(sessionB);
  LocalLiteTxnManager::destroyContext(observer);
  printf("local-lite session transaction probe passed\n");
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
