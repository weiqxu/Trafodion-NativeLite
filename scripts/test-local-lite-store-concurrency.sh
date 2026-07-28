#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cxx=${CXX:-/usr/bin/g++}
[[ -x "$cxx" ]] || command -v "$cxx" >/dev/null 2>&1 || fail "missing C++ compiler: $cxx"

work_dir=$(mktemp -d /tmp/traf-local-lite-store-concurrency.XXXXXX)
store_dir="$work_dir/store"
src="$work_dir/local_lite_store_concurrency.cpp"
bin="$work_dir/local_lite_store_concurrency"
trap 'rm -rf "$work_dir"' EXIT

cat >"$src" <<'CPP'
#include "LocalLiteRocksDBStore.h"

#include <algorithm>
#include <pthread.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
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

struct SharedState
{
  LocalLiteTableDef table;
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
  LocalLiteRocksDBStore store;
  LocalLiteTxn txn(&store);
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
  LocalLiteRocksDBStore store;
  LocalLiteTxn txn(&store);
  size_t previousCount = 0;
  for (;;)
    {
      pthread_mutex_lock(&state->mutex);
      bool done = state->writersDone;
      bool failed = state->failed;
      pthread_mutex_unlock(&state->mutex);
      if (failed)
        return NULL;

      std::vector<LocalLiteRow> rows;
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
  LocalLiteColumnDef col;
  col.name = "A";
  col.type = "VARCHAR(32)";
  col.nullable = true;
  state.table.columns.push_back(col);

  LocalLiteRocksDBStore setupStore;
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

  LocalLiteRocksDBStore verifyStore;
  LocalLiteTxn verifyTxn(&verifyStore);
  std::vector<LocalLiteRow> rows;
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

  printf("local-lite store concurrency probe passed\n");
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
