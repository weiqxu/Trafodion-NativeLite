#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
run_root=$(mktemp -d /tmp/traf-lite-storage-contract.XXXXXX)
trap 'rm -rf "$run_root"' EXIT
cxx=${CXX:-/usr/bin/g++}

"$cxx" -std=c++11 -Wall -Wextra -Werror -pthread \
  -I"${ROCKSDB_INC_DIR:-/usr/include}" \
  -I"$repo_root/core/sql/litestore" \
  "$repo_root/core/sql/litestore/LiteStorage.cpp" \
  "$repo_root/core/sql/litestore/LiteStorageSQLite.cpp" \
  "$repo_root/scripts/lite-storage-contract-test.cpp" \
  -L"${ROCKSDB_LIB_DIR:-/usr/lib/x86_64-linux-gnu}" \
  -lrocksdb -lsqlite3 -o "$run_root/storage-contract-test"

"$run_root/storage-contract-test" rocksdb \
  "$run_root/rocksdb" "$run_root/rocksdb-checkpoint" \
  "$run_root/rocksdb-backup" "$run_root/rocksdb-restored"
"$run_root/storage-contract-test" sqlite \
  "$run_root/sqlite.db" "$run_root/sqlite-checkpoint.db" \
  "$run_root/sqlite-backup.db" "$run_root/sqlite-restored.db"
