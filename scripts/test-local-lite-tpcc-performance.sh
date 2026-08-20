#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/nativelite-server"
bulk_loader="$repo_root/core/sqf/export/bin64d/nativelite-bulk-loader"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
loader_source="$repo_root/scripts/NativeLiteTpcc.java"
transaction_source="$repo_root/scripts/NativeLiteTpccTransactions.java"
workload_source="$repo_root/scripts/NativeLiteTpccWorkload.java"
properties=${TPCC_PROPERTIES:-$repo_root/benchmarks/tpcc/qualification.properties}
tpcc_scale=${TPCC_SCALE:-multi}
schema="$repo_root/benchmarks/tpcc/schema.sql"
build_type=${LOCAL_LITE_BUILD_TYPE:-debug}
if [[ "$build_type" == "release" ]]; then
  server="$repo_root/core/sqf/export/bin64d/nativelite-server"
  sql_libs="$repo_root/core/sql/lib/linux/64bit/release"
  sqf_libs="$repo_root/core/sqf/export/lib64d"
else
  server="$repo_root/core/sqf/export/bin64d/nativelite-server"
  sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
  sqf_libs="$repo_root/core/sqf/export/lib64d"
fi
traf_home="$repo_root/core/sqf"
workers=${NATIVELITE_WORKERS:-32}
native_bulk_load=${TPCC_NATIVE_BULK_LOAD:-0}
native_commit_rows=${TPCC_NATIVE_COMMIT_ROWS:-}

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$server" ]] || fail "missing built NativeLite server: $server"
if [[ "$native_bulk_load" == "1" ]]; then
  [[ -x "$bulk_loader" ]] || fail "missing NativeLite bulk loader: $bulk_loader"
fi

slf4j_jar=${SLF4J_API_JAR:-}
if [[ -z "$slf4j_jar" && -r /usr/share/java/slf4j-api.jar ]]; then
  slf4j_jar=/usr/share/java/slf4j-api.jar
fi
if [[ -z "$slf4j_jar" ]]; then
  shopt -s nullglob
  candidates=("${HOME}"/.m2/repository/org/slf4j/slf4j-api/*/slf4j-api-*.jar)
  shopt -u nullglob
  ((${#candidates[@]} > 0)) || fail "SLF4J API jar not found"
  slf4j_jar=${candidates[${#candidates[@]}-1]}
fi

if [[ -n "${TPCC_TEST_ROOT:-}" ]]; then
  test_root=$TPCC_TEST_ROOT
  mkdir -p "$test_root"
else
  test_root=$(mktemp -d /tmp/traf-local-lite-tpcc-performance.XXXXXX)
fi
primary_store="$test_root/primary-store"
checkpoint_store="$test_root/checkpoint-store"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
workload_report="$test_root/workload-report.json"
operations_report="$test_root/operations-report.json"
mkdir -p "$primary_store" "$checkpoint_store" "$classes_dir"
active_store="$primary_store"
server_pid=
workload_pid=
port=${NATIVELITE_TEST_PORT:-$((29000 + ($$ % 8000)))}
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"

cleanup() {
  rc=$?
  if [[ -n "${workload_pid:-}" ]] && kill -0 "$workload_pid" 2>/dev/null; then
    kill -TERM "$workload_pid" 2>/dev/null || true
    wait "$workload_pid" 2>/dev/null || true
  fi
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" -ne 0 ]]; then
    echo "M14F artifacts retained at $test_root" >&2
    [[ ! -s "$server_log" ]] || sed -n '1,360p' "$server_log" >&2
  elif [[ "${TPCC_KEEP_TEST_ROOT:-0}" == "1" ]]; then
    echo "TPCC artifacts retained at $test_root"
  elif [[ -n "${TPCC_ARTIFACT_DIR:-${TPCC_M14F_ARTIFACT_DIR:-}}" ]]; then
    artifact_dir=${TPCC_ARTIFACT_DIR:-$TPCC_M14F_ARTIFACT_DIR}
    mkdir -p "$artifact_dir"
    for evidence in workload-report.json operations-report.json bulk-load.json \
      bulk-load.manifest load.json checkpoint.json live-verify.json \
      clean-restart.json unclean-restart.json checkpoint-restore.json \
      watermark.json workload.stdout server.log; do
      [[ ! -s "$test_root/$evidence" ]] ||
        cp -a "$test_root/$evidence" "$artifact_dir/"
    done
    cp -a "$properties" "$artifact_dir/qualification.properties"
    rm -rf "$test_root"
  else
    rm -rf "$test_root"
  fi
}
trap cleanup EXIT

start_server() {
  local minimum_free=${1:-}
  : >"$server_log"
  if [[ -n "$minimum_free" ]]; then
    env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
      TRAF_LOCAL_STORE_DIR="$active_store" \
      TRAF_LOCAL_LITE_MINIMUM_FREE_BYTES="$minimum_free" \
      TRAF_LOCAL_LITE_CHECKPOINT_DIR="$checkpoint_store/transactiondb" \
      LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
      "$server" --listen 127.0.0.1 --port "$port" --workers "$workers" >"$server_log" 2>&1 &
  else
    env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
      TRAF_LOCAL_STORE_DIR="$active_store" \
      TRAF_LOCAL_LITE_CHECKPOINT_DIR="$checkpoint_store/transactiondb" \
      LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
      "$server" --listen 127.0.0.1 --port "$port" --workers "$workers" >"$server_log" 2>&1 &
  fi
  server_pid=$!
  for _ in $(seq 1 300); do
    if grep -q 'NativeLite server ready' "$server_log"; then return; fi
    kill -0 "$server_pid" 2>/dev/null || fail "M14F server exited at startup"
    sleep 0.05
  done
  fail "M14F server did not become ready"
}

stop_server() {
  kill -TERM "$server_pid"
  wait "$server_pid"
  server_pid=
}

find "$driver_source" -name '*.java' -print0 | \
  xargs -0 javac -nowarn -cp "$slf4j_jar" -d "$classes_dir"
javac -Xlint:all -cp "$classes_dir:$slf4j_jar" -d "$classes_dir" \
  "$loader_source" "$transaction_source" "$workload_source"

if [[ "${TPCC_REUSE_LOADED_STORE:-0}" == "diagnostic" ]]; then
  [[ -s "$test_root/bulk-load.json" &&
      -s "$primary_store/transactiondb/CURRENT" ]] ||
    fail "requested diagnostic store reuse has no completed native load"
  start_server
elif [[ "${TPCC_REUSE_LOADED_STORE:-0}" == "1" ]]; then
  [[ -s "$test_root/bulk-load.json" &&
      -s "$primary_store/transactiondb/CURRENT" ]] ||
    fail "requested loaded-store reuse has no completed native load"
  start_server
  java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
    "$jdbc_url" verify "$tpcc_scale" "$properties" "$schema" \
    "$test_root/load.json"
elif [[ "$native_bulk_load" == "1" ]]; then
  # Create the catalog through the supported SQL path, then close the server
  # before the native loader opens the same unified RocksDB store.
  start_server
  java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
    "$jdbc_url" schema "$tpcc_scale" "$properties" "$schema" \
    "$test_root/schema.json"
  stop_server
  bulk_loader_args=(--properties "$properties" --scale "$tpcc_scale")
  if [[ -n "$native_commit_rows" ]]; then
    bulk_loader_args+=(--commit-rows "$native_commit_rows")
  fi
  bulk_loader_args+=(--manifest "$test_root/bulk-load.manifest")
  env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
    TRAF_LOCAL_LITE_BULK_LOAD=1 \
    TRAF_LOCAL_STORE_DIR="$active_store" \
    TRAF_LOCAL_LITE_CHECKPOINT_DIR="$checkpoint_store/transactiondb" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$bulk_loader" "${bulk_loader_args[@]}" \
    --report "$test_root/bulk-load.json"
  start_server
  java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
    "$jdbc_url" verify "$tpcc_scale" "$properties" "$schema" \
    "$test_root/load.json"
else
  start_server
  java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
    "$jdbc_url" load "$tpcc_scale" "$properties" "$schema" "$test_root/load.json"
fi
# Full relationship verification materializes the largest tables in the
# reduced executor. Start the measured phase from a clean process so those
# transient rows cannot consume the workload's memory budget; the persisted
# TransactionDB and synchronous-commit state remain unchanged.
stop_server
start_server
rss_before=$(awk '/VmRSS:/ {print $2}' "/proc/$server_pid/status")
store_bytes_before=$(du -sb "$primary_store" | awk '{print $1}')
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" run "$properties" "$workload_report" \
  >"$test_root/workload.stdout" 2>&1 &
workload_pid=$!
sleep 1
kill -0 "$workload_pid" 2>/dev/null || fail "workload ended before online checkpoint"
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" checkpoint "$properties" "$test_root/checkpoint.json"
wait "$workload_pid"
workload_pid=
grep -q '"unclassified_errors":0' "$workload_report" ||
  fail "M14F workload has unclassified errors"
rss_after=$(awk '/VmHWM:/ {print $2}' "/proc/$server_pid/status")
store_bytes=$(du -sb "$primary_store" | awk '{print $1}')
store_delta_bytes=$((store_bytes - store_bytes_before))
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" verify "$properties" "$test_root/live-verify.json"

stop_server
clean_started=$(date +%s%N)
start_server
clean_recovery_ms=$((($(date +%s%N) - clean_started) / 1000000))
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" verify "$properties" "$test_root/clean-restart.json"

kill -KILL "$server_pid"
set +e
wait "$server_pid"
unclean_rc=$?
set -e
server_pid=
[[ "$unclean_rc" -eq 137 ]] || fail "unclean stop rc=$unclean_rc"
unclean_started=$(date +%s%N)
start_server
unclean_recovery_ms=$((($(date +%s%N) - unclean_started) / 1000000))
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" verify "$properties" "$test_root/unclean-restart.json"
stop_server

active_store="$checkpoint_store"
checkpoint_started=$(date +%s%N)
start_server
checkpoint_recovery_ms=$((($(date +%s%N) - checkpoint_started) / 1000000))
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
  "$jdbc_url" verify "$properties" "$test_root/checkpoint-restore.json"
stop_server

active_store="$primary_store"
: >"$server_log"
env TRAF_HOME="$traf_home" TRAF_LOCAL_LITE=1 \
  TRAF_LOCAL_STORE_DIR="$active_store" \
  TRAF_LOCAL_LITE_MINIMUM_FREE_BYTES=18446744073709551615 \
  TRAF_LOCAL_LITE_CHECKPOINT_DIR="$checkpoint_store/transactiondb" \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --listen 127.0.0.1 --port "$port" --workers "$workers" >"$server_log" 2>&1 &
server_pid=$!
for _ in $(seq 1 300); do
  if grep -q 'NativeLite server ready' "$server_log"; then
    java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccWorkload \
      "$jdbc_url" watermark "$properties" "$test_root/watermark.json"
    stop_server
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    set +e
    wait "$server_pid"
    watermark_rc=$?
    set -e
    server_pid=
    [[ "$watermark_rc" -ne 0 ]] || fail "watermark startup unexpectedly passed"
    grep -q 'storage disk watermark reached' "$server_log" ||
      fail "watermark startup failed without the expected diagnostic"
    printf '{"disk_watermark_startup_rejection":"pass"}\n' \
      >"$test_root/watermark.json"
    break
  fi
  sleep 0.05
done
[[ -s "$test_root/watermark.json" ]] || fail "watermark gate did not finish"

printf '{"contract_version":1,"environment":{"architecture":"%s","kernel_release":"%s","java_version":"%s","source_revision":"%s"},"rss_kib":{"after_load":%s,"high_water":%s},"store_bytes":{"after_load":%s,"after_workload":%s,"delta":%s},"recovery_ms":{"clean":%s,"unclean":%s,"checkpoint":%s},"online_checkpoint":"pass","clean_restart":"pass","unclean_restart":"pass","checkpoint_restore":"pass","disk_watermark":"pass"}\n' \
  "$(uname -m)" "$(uname -r)" \
  "$(java -XshowSettings:properties -version 2>&1 | awk -F'= ' '/java.version =/ {print $2; exit}')" \
  "$(git -C "$repo_root" rev-parse --short=12 HEAD)" "$rss_before" \
  "$rss_after" "$store_bytes_before" "$store_bytes" "$store_delta_bytes" \
  "$clean_recovery_ms" "$unclean_recovery_ms" "$checkpoint_recovery_ms" \
  >"$operations_report"
cat "$workload_report"
cat "$operations_report"
echo "LocalLite M14F multi-warehouse performance and operations checks passed"
