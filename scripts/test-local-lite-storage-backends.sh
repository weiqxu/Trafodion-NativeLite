#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
run_root=$(mktemp -d /tmp/traf-local-lite-storage-backends.XXXXXX)
trap 'rm -rf "$run_root"' EXIT
cxx=${CXX:-/usr/bin/g++}

"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -I"${ROCKSDB_INC_DIR:-/usr/include}" \
  -I"$repo_root/core/sql/localstore" \
  "$repo_root/core/sql/localstore/LocalLiteStorage.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteStorageSQLite.cpp" \
  "$repo_root/scripts/local-lite-storage-benchmark.cpp" \
  -L"${ROCKSDB_LIB_DIR:-/usr/lib/x86_64-linux-gnu}" \
  -lrocksdb -lsqlite3 -o "$run_root/storage-benchmark"

"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -I"${ROCKSDB_INC_DIR:-/usr/include}" \
  -I"$repo_root/core/sql/localstore" \
  "$repo_root/core/sql/localstore/LocalLiteStorage.cpp" \
  "$repo_root/core/sql/localstore/LocalLiteStorageSQLite.cpp" \
  "$repo_root/scripts/local-lite-storage-crash-test.cpp" \
  -L"${ROCKSDB_LIB_DIR:-/usr/lib/x86_64-linux-gnu}" \
  -lrocksdb -lsqlite3 -o "$run_root/storage-crash-test"

for backend in rocksdb sqlite; do
  store_path="$run_root/$backend-crash"
  [[ $backend == sqlite ]] && store_path="$store_path.db"
  "$run_root/storage-crash-test" "$backend" committed "$store_path"
  set +e
  "$run_root/storage-crash-test" "$backend" uncommitted "$store_path"
  crash_rc=$?
  set -e
  [[ $crash_rc -eq 77 ]] || {
    echo "FAIL: $backend uncommitted crash probe rc=$crash_rc" >&2
    exit 1
  }
  "$run_root/storage-crash-test" "$backend" verify "$store_path"
  echo "backend=$backend crash_recovery=pass"

  set +e
  TRAF_LOCAL_LITE_STORAGE_FAULT=before-commit \
    "$run_root/storage-crash-test" "$backend" fault-commit "$store_path"
  before_rc=$?
  set -e
  [[ $before_rc -eq 86 ]] || {
    echo "FAIL: $backend before-commit fault rc=$before_rc" >&2
    exit 1
  }
  "$run_root/storage-crash-test" "$backend" verify-fault-absent "$store_path"

  set +e
  TRAF_LOCAL_LITE_STORAGE_FAULT=after-commit \
    "$run_root/storage-crash-test" "$backend" fault-commit "$store_path"
  after_rc=$?
  set -e
  [[ $after_rc -eq 87 ]] || {
    echo "FAIL: $backend after-commit fault rc=$after_rc" >&2
    exit 1
  }
  "$run_root/storage-crash-test" "$backend" verify-fault-present "$store_path"

  for operation in checkpoint backup; do
    set +e
    TRAF_LOCAL_LITE_STORAGE_FAULT=$operation \
      "$run_root/storage-crash-test" "$backend" "$operation" "$store_path"
    operation_rc=$?
    set -e
    expected_rc=88
    [[ $operation == backup ]] && expected_rc=89
    [[ $operation_rc -eq $expected_rc ]] || {
      echo "FAIL: $backend $operation fault rc=$operation_rc" >&2
      exit 1
    }
    "$run_root/storage-crash-test" "$backend" verify-fault-present \
      "$store_path"
  done
  echo "backend=$backend fault_matrix=pass"
done

transactions=${LOCAL_LITE_M12_BENCHMARK_TXNS:-300}
rocksdb_result=$("$run_root/storage-benchmark" rocksdb \
  "$run_root/rocksdb" "$transactions")
sqlite_result=$("$run_root/storage-benchmark" sqlite \
  "$run_root/sqlite.db" "$transactions")
echo "$rocksdb_result"
echo "$sqlite_result"

extract_metric() {
  local line=$1 name=$2 field
  for field in $line; do
    if [[ $field == "$name="* ]]; then
      echo "${field#*=}"
      return 0
    fi
  done
  return 1
}

rocksdb_p99=$(extract_metric "$rocksdb_result" p99_us)
sqlite_p99=$(extract_metric "$sqlite_result" p99_us)
[[ $rocksdb_p99 -gt 0 && $sqlite_p99 -gt 0 ]] || {
  echo "FAIL: backend benchmark did not report p99 latency" >&2
  exit 1
}

# Both candidates must first pass the identical correctness contract (the
# preceding M12A target). TransactionDB remains selected because it supplies
# pessimistic key-level conflicts, native checkpoint/backup, existing RocksDB
# on-disk tooling, and the lowest-risk migration from the current LocalLite
# store. The measured p95/p99 values are evidence, not a machine-specific hard
# performance threshold.
echo "selected=rocksdb-transactiondb reason=correctness,recovery,operational-continuity"
