#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work_dir=$(mktemp -d /tmp/traf-local-lite-occ-validation.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT

cxx=${CXX:-/usr/bin/g++}
rocksdb_include_flags=(-I/usr/include)
rocksdb_library_flags=()
[[ ! -d /usr/include/x86_64-linux-gnu ]] || \
  rocksdb_include_flags+=(-I/usr/include/x86_64-linux-gnu)
[[ ! -d /usr/lib/x86_64-linux-gnu ]] || \
  rocksdb_library_flags+=(-L/usr/lib/x86_64-linux-gnu)
[[ ! -d /lib/x86_64-linux-gnu ]] || \
  rocksdb_library_flags+=(-L/lib/x86_64-linux-gnu)

"$cxx" -DTRAF_LOCAL_LITE -std=c++0x \
  -I"$repo_root/core/sql/localstore" \
  "${rocksdb_include_flags[@]}" \
  "$repo_root/scripts/local-lite-occ-validation-test.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteStorage.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteUnifiedRocksDB.cpp" \
  "$repo_root/scripts/local-lite-row-codec-test-stubs.cpp" \
  "${rocksdb_library_flags[@]}" -lrocksdb -lpthread \
  -o "$work_dir/local-lite-occ-validation"

TRAF_LOCAL_STORE_DIR="$work_dir/store" \
  "$work_dir/local-lite-occ-validation"
