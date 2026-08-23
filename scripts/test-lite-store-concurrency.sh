#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cxx=${CXX:-/usr/bin/g++}
[[ -x "$cxx" ]] || command -v "$cxx" >/dev/null 2>&1 || fail "missing C++ compiler: $cxx"

work_dir=$(mktemp -d /tmp/traf-lite-store-concurrency.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/lite_store_concurrency.cpp"
bin="$work_dir/lite_store_concurrency"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LiteRocksDBStore.h"

#include <algorithm>
#include <pthread.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
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
                             size_t keyOrdinal,
                             std::string *key,
                             bool *hasKey,
                             std::string *error)
{
  if (keyColumns.empty())
    {
      if (hasKey)
        *hasKey = false;
      if (error)
        error->clear();
      return true;
    }
  char ordinal[32];
  snprintf(ordinal, sizeof(ordinal), "%lu",
           static_cast<unsigned long>(keyOrdinal));
  *key = "U" + std::string(ordinal) + ":" + encodedRow;
  if (hasKey)
    *hasKey = true;
  if (error)
    error->clear();
  return true;
}

bool LiteBuildOrderedSecondaryKeyPayload(
    const LiteTableDef &,
    const LiteIndexDef &,
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
    *error = "ordered index codec is not used by the concurrency probe";
  return false;
}

struct SharedState
{
  LiteTableDef table;
  pthread_mutex_t mutex;
  bool writersDone;
  bool failed;
  std::string error;
};

struct WriterArg
{
  SharedState *state;
  int writer;
  int rows;
};

struct ConflictState
{
  LiteTableDef table;
  std::string payload;
  std::string expectedError;
  pthread_mutex_t mutex;
  pthread_barrier_t barrier;
  int successes;
  int expectedConflicts;
  bool failed;
  std::string error;
};

static void recordError(SharedState *state, const std::string &error)
{
  pthread_mutex_lock(&state->mutex);
  state->failed = true;
  if (state->error.empty())
    state->error = error;
  pthread_mutex_unlock(&state->mutex);
}

static void *writerMain(void *arg)
{
  WriterArg *writerArg = static_cast<WriterArg *>(arg);
  SharedState *state = writerArg->state;
  TxnContextOwner session;
  LiteRocksDBStore store;
  LiteTxn txn(&store, session.get());
  for (int i = 0; i < writerArg->rows; i++)
    {
      char payload[64];
      snprintf(payload, sizeof(payload), "writer-%02d-row-%03d",
               writerArg->writer, i);
      uint64_t rowId = 0;
      std::string error;
      if (!txn.insertRow(state->table, payload, &rowId, &error))
        {
          recordError(state, error);
          return NULL;
        }
      if (rowId == 0)
        {
          recordError(state, "insert returned row id 0");
          return NULL;
        }
    }
  return NULL;
}

static void *scannerMain(void *arg)
{
  SharedState *state = static_cast<SharedState *>(arg);
  TxnContextOwner session;
  LiteRocksDBStore store;
  LiteTxn txn(&store, session.get());
  size_t previousCount = 0;
  for (;;)
    {
      pthread_mutex_lock(&state->mutex);
      bool done = state->writersDone;
      bool failed = state->failed;
      pthread_mutex_unlock(&state->mutex);
      if (failed)
        return NULL;

      std::vector<LiteRow> rows;
      std::string error;
      if (!txn.scanRows(state->table, &rows, &error))
        {
          recordError(state, error);
          return NULL;
        }
      if (rows.size() < previousCount)
        {
          recordError(state, "scan row count moved backwards");
          return NULL;
        }
      previousCount = rows.size();
      if (done)
        return NULL;
    }
}

static void *conflictWriterMain(void *arg)
{
  ConflictState *state = static_cast<ConflictState *>(arg);
  TxnContextOwner session;
  LiteRocksDBStore store;
  LiteTxn txn(&store, session.get());
  pthread_barrier_wait(&state->barrier);

  uint64_t rowId = 0;
  std::string error;
  bool inserted = txn.insertRow(state->table, state->payload, &rowId, &error);

  pthread_mutex_lock(&state->mutex);
  if (inserted)
    state->successes++;
  else if (error == state->expectedError)
    state->expectedConflicts++;
  else
    {
      state->failed = true;
      if (state->error.empty())
        state->error = error;
    }
  pthread_mutex_unlock(&state->mutex);
  return NULL;
}

static bool runConflictProbe(const LiteTableDef &table,
                             const std::string &payload,
                             const std::string &expectedError,
                             std::string *error)
{
  const int writerCount = 8;
  ConflictState state;
  state.table = table;
  state.payload = payload;
  state.expectedError = expectedError;
  state.successes = 0;
  state.expectedConflicts = 0;
  state.failed = false;
  pthread_mutex_init(&state.mutex, NULL);
  pthread_barrier_init(&state.barrier, NULL, writerCount);

  pthread_t writers[writerCount];
  for (int i = 0; i < writerCount; i++)
    if (pthread_create(&writers[i], NULL, conflictWriterMain, &state) != 0)
      {
        *error = "create conflict writer thread failed";
        return false;
      }
  for (int i = 0; i < writerCount; i++)
    pthread_join(writers[i], NULL);

  pthread_barrier_destroy(&state.barrier);
  pthread_mutex_destroy(&state.mutex);
  if (state.failed)
    {
      *error = state.error;
      return false;
    }
  if (state.successes != 1 || state.expectedConflicts != writerCount - 1)
    {
      char counts[128];
      snprintf(counts, sizeof(counts),
               "expected 1 success and %d conflicts, found %d and %d",
               writerCount - 1, state.successes, state.expectedConflicts);
      *error = counts;
      return false;
    }
  return true;
}

int main()
{
  SharedState state;
  pthread_mutex_init(&state.mutex, NULL);
  state.writersDone = false;
  state.failed = false;

  state.table.catalog = "TRAFODION";
  state.table.schema = "SEABASE";
  state.table.name = "CONC_T";
  state.table.objectUid = 1001;
  state.table.nextRowId = 1;
  LiteColumnDef col;
  col.name = "A";
  col.type = "VARCHAR(32)";
  col.nullable = true;
  state.table.columns.push_back(col);

  LiteRocksDBStore setupStore;
  std::string error;
  if (!setupStore.createTable(state.table, &error))
    {
      fprintf(stderr, "create table failed: %s\n", error.c_str());
      return 1;
    }

  const int writerCount = 6;
  const int rowsPerWriter = 40;
  pthread_t writers[writerCount];
  WriterArg args[writerCount];
  pthread_t scanner;

  if (pthread_create(&scanner, NULL, scannerMain, &state) != 0)
    {
      fprintf(stderr, "create scanner thread failed\n");
      return 1;
    }

  for (int i = 0; i < writerCount; i++)
    {
      args[i].state = &state;
      args[i].writer = i;
      args[i].rows = rowsPerWriter;
      if (pthread_create(&writers[i], NULL, writerMain, &args[i]) != 0)
        {
          fprintf(stderr, "create writer thread failed\n");
          return 1;
        }
    }

  for (int i = 0; i < writerCount; i++)
    pthread_join(writers[i], NULL);

  pthread_mutex_lock(&state.mutex);
  state.writersDone = true;
  pthread_mutex_unlock(&state.mutex);
  pthread_join(scanner, NULL);

  pthread_mutex_lock(&state.mutex);
  bool failed = state.failed;
  std::string failure = state.error;
  pthread_mutex_unlock(&state.mutex);
  if (failed)
    {
      fprintf(stderr, "%s\n", failure.c_str());
      return 1;
    }

  LiteRocksDBStore verifyStore;
  TxnContextOwner verifySession;
  LiteTxn verifyTxn(&verifyStore, verifySession.get());
  std::vector<LiteRow> rows;
  if (!verifyTxn.scanRows(state.table, &rows, &error))
    {
      fprintf(stderr, "final scan failed: %s\n", error.c_str());
      return 1;
    }

  const size_t expectedRows = writerCount * rowsPerWriter;
  if (rows.size() != expectedRows)
    {
      fprintf(stderr, "expected %zu rows, found %zu\n",
              expectedRows, rows.size());
      return 1;
    }

  std::set<uint64_t> rowIds;
  for (size_t i = 0; i < rows.size(); i++)
    rowIds.insert(rows[i].rowId);
  if (rowIds.size() != expectedRows)
    {
      fprintf(stderr, "duplicate row ids detected\n");
      return 1;
    }
  if (*rowIds.begin() != 1 || *rowIds.rbegin() != expectedRows)
    {
      fprintf(stderr, "row ids are not contiguous from 1 to %zu\n",
              expectedRows);
      return 1;
    }

  LiteTableDef primaryTable = state.table;
  primaryTable.name = "CONC_PK_T";
  primaryTable.objectUid = 1002;
  primaryTable.nextRowId = 1;
  primaryTable.primaryKeyColumns.push_back(0);
  primaryTable.primaryKeyName = "CONC_PK";
  if (!setupStore.createTable(primaryTable, &error))
    {
      fprintf(stderr, "create primary-key table failed: %s\n", error.c_str());
      return 1;
    }
  if (!runConflictProbe(primaryTable, "same-primary-key",
                        "duplicate lite primary key", &error))
    {
      fprintf(stderr, "primary-key conflict probe failed: %s\n", error.c_str());
      return 1;
    }
  rows.clear();
  if (!verifyTxn.scanRows(primaryTable, &rows, &error) ||
      rows.size() != 1 || rows[0].value != "same-primary-key")
    {
      fprintf(stderr, "primary-key conflict changed persisted rows: %s\n",
              error.c_str());
      return 1;
    }

  LiteTableDef uniqueTable = state.table;
  uniqueTable.name = "CONC_UQ_T";
  uniqueTable.objectUid = 1003;
  uniqueTable.nextRowId = 1;
  uniqueTable.uniqueKeyColumns.push_back(std::vector<size_t>(1, 0));
  uniqueTable.uniqueKeyNames.push_back("CONC_UQ");
  if (!setupStore.createTable(uniqueTable, &error))
    {
      fprintf(stderr, "create unique-key table failed: %s\n", error.c_str());
      return 1;
    }
  if (!runConflictProbe(uniqueTable, "same-unique-key",
                        "duplicate lite unique key", &error))
    {
      fprintf(stderr, "unique-key conflict probe failed: %s\n", error.c_str());
      return 1;
    }
  LiteTableDef loadedUnique;
  if (!setupStore.loadTable(uniqueTable.catalog, uniqueTable.schema,
                            uniqueTable.name, &loadedUnique, &error) ||
      loadedUnique.nextRowId != 2)
    {
      fprintf(stderr, "unique conflicts advanced keyless row ids: %s\n",
              error.c_str());
      return 1;
    }
  uint64_t nextUniqueRowId = 0;
  if (!verifyTxn.insertRow(uniqueTable, "different-unique-key",
                           &nextUniqueRowId, &error) ||
      nextUniqueRowId != 2)
    {
      fprintf(stderr, "unique conflict left row-id metadata unusable: %s\n",
              error.c_str());
      return 1;
    }

  printf("lite store concurrency probe passed\n");
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
