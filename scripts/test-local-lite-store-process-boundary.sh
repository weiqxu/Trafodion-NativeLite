#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cxx=${CXX:-/usr/bin/g++}
[[ -x "$cxx" ]] || command -v "$cxx" >/dev/null 2>&1 || fail "missing C++ compiler: $cxx"

work_dir=$(mktemp -d /tmp/traf-local-lite-store-boundary.XXXXXX)
store_dir="$work_dir/store"
ready_file="$work_dir/ready"
src="$work_dir/local_lite_store_boundary.cpp"
bin="$work_dir/local_lite_store_boundary"
holder_pid=""

cleanup() {
  if [[ -n "$holder_pid" ]] && kill -0 "$holder_pid" >/dev/null 2>&1; then
    kill "$holder_pid" >/dev/null 2>&1 || true
    wait "$holder_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT

cat >"$src" <<'CPP'
#include "LocalLiteRocksDBStore.h"

#include <pthread.h>
#include <stdio.h>
#include <string>
#include <unistd.h>
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

static LocalLiteTableDef tableDef()
{
  LocalLiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  table.name = "BOUNDARY_T";
  table.objectUid = 2001;
  table.nextRowId = 1;
  LocalLiteColumnDef col;
  col.name = "A";
  col.type = "VARCHAR(32)";
  col.nullable = true;
  table.columns.push_back(col);
  return table;
}

int main(int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf(stderr, "usage: %s hold <ready-file> | open\n", argv[0]);
      return 2;
    }

  LocalLiteRocksDBStore store;
  std::string error;
  if (std::string(argv[1]) == "hold")
    {
      if (argc < 3)
        return 2;
      if (!store.createTable(tableDef(), &error))
        {
          fprintf(stderr, "%s\n", error.c_str());
          return 1;
        }
      FILE *ready = fopen(argv[2], "w");
      if (!ready)
        {
          perror("ready");
          return 1;
        }
      fputs("ready\n", ready);
      fclose(ready);
      sleep(30);
      return 0;
    }

  bool exists = false;
  if (!store.tableExists("TRAFODION", "SEABASE", "BOUNDARY_T",
                         &exists, &error))
    {
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  return exists ? 0 : 1;
}
CPP

include_flags=()
if [[ -d /usr/include/x86_64-linux-gnu ]]; then
  include_flags+=(-I/usr/include/x86_64-linux-gnu)
fi
include_flags+=(-I/usr/include)
library_flags=()
if [[ -d /usr/lib/x86_64-linux-gnu ]]; then
  library_flags+=(-L/usr/lib/x86_64-linux-gnu)
fi
if [[ -d /lib/x86_64-linux-gnu ]]; then
  library_flags+=(-L/lib/x86_64-linux-gnu)
fi

"$cxx" -DTRAF_LOCAL_LITE -std=c++0x \
  -I"$repo_root/core/sql/localstore" \
  "${include_flags[@]}" \
  "$src" "$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp" \
  "${library_flags[@]}" \
  -lrocksdb -lpthread -o "$bin"

TRAF_LOCAL_STORE_DIR="$store_dir" "$bin" hold "$ready_file" &
holder_pid=$!

for _ in $(seq 1 50); do
  [[ -f "$ready_file" ]] && break
  if ! kill -0 "$holder_pid" >/dev/null 2>&1; then
    wait "$holder_pid" || true
    fail "local-lite boundary holder exited before ready"
  fi
  sleep 0.1
done
[[ -f "$ready_file" ]] || fail "local-lite boundary holder did not become ready"

set +e
boundary_output=$(TRAF_LOCAL_STORE_DIR="$store_dir" "$bin" open 2>&1)
boundary_status=$?
set -e

[[ "$boundary_status" -ne 0 ]] ||
  fail "second local-lite process unexpectedly opened the same store"
grep -q 'local-lite store is already open by another process' <<<"$boundary_output" ||
  fail "cross-process store diagnostic missing: $boundary_output"
grep -q 'TRAF_LOCAL_STORE_DIR' <<<"$boundary_output" ||
  fail "cross-process store diagnostic must name TRAF_LOCAL_STORE_DIR"

echo "local-lite store process boundary probe passed"
