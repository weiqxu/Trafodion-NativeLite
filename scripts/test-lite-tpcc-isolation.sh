#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/nativelite-server"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
loader_source="$repo_root/scripts/NativeLiteTpcc.java"
transaction_source="$repo_root/scripts/NativeLiteTpccTransactions.java"
isolation_source="$repo_root/scripts/NativeLiteTpccIsolation.java"
properties="$repo_root/benchmarks/tpcc/qualification.properties"
schema="$repo_root/benchmarks/tpcc/schema.sql"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$server" ]] || fail "missing built NativeLite server: $server"
command -v javac >/dev/null || fail "javac is required for the M14D gate"
command -v java >/dev/null || fail "java is required for the M14D gate"

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

test_root=$(mktemp -d /tmp/traf-lite-m14d.XXXXXX)
base_store="$test_root/base-store"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
load_report="$test_root/load-report.json"
isolation_report="$test_root/isolation-report.json"
mkdir -p "$base_store" "$classes_dir"
active_store="$base_store"
server_pid=
port=${NATIVELITE_TEST_PORT:-$((27000 + ($$ % 10000)))}
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"

cleanup() {
  rc=$?
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" -ne 0 && -s "$server_log" ]]; then
    echo "NativeLite M14D server log:" >&2
    sed -n '1,300p' "$server_log" >&2
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

start_server() {
  local fault=${1:-}
  : >"$server_log"
  if [[ -n "$fault" ]]; then
    env TRAF_HOME="$traf_home" TRAF_LITE=1 \
      TRAF_LITE_STORE_DIR="$active_store" \
      TRAF_LITE_COMMIT_FAULT="$fault" \
      LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
      "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
  else
    env TRAF_HOME="$traf_home" TRAF_LITE=1 \
      TRAF_LITE_STORE_DIR="$active_store" \
      LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
      "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
  fi
  server_pid=$!
  for _ in $(seq 1 200); do
    if grep -q 'NativeLite server ready' "$server_log"; then return; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      cat "$server_log" >&2
      fail "NativeLite server exited during M14D startup"
    fi
    sleep 0.05
  done
  fail "NativeLite server did not become ready for M14D"
}

stop_server() {
  kill -TERM "$server_pid"
  set +e
  wait "$server_pid"
  local rc=$?
  set -e
  server_pid=
  [[ "$rc" -eq 0 ]] || fail "NativeLite graceful shutdown returned $rc"
}

find "$driver_source" -name '*.java' -print0 | \
  xargs -0 javac -nowarn -cp "$slf4j_jar" -d "$classes_dir"
javac -Xlint:all -cp "$classes_dir:$slf4j_jar" -d "$classes_dir" \
  "$loader_source" "$transaction_source" "$isolation_source"

start_server
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
  "$jdbc_url" load smoke "$properties" "$schema" "$load_report"
stop_server

active_store="$test_root/isolation-store"
mkdir -p "$active_store"
cp -a "$base_store/." "$active_store/"
start_server
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccIsolation \
  "$jdbc_url" "$isolation_report"
grep -q '"write_skew":"pass"' "$isolation_report" ||
  fail "M14D isolation matrix did not pass"
stop_server

for profile in new-order payment delivery; do
  for point in before after; do
    active_store="$test_root/${profile}-${point}-store"
    mkdir -p "$active_store"
    cp -a "$base_store/." "$active_store/"
    if [[ "$point" == before ]]; then
      fault=before-batch
      expected_server_rc=92
    else
      fault=after-batch
      expected_server_rc=93
    fi
    start_server "$fault"
    set +e
    java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccTransactions \
      "$jdbc_url" "fault-${profile}" "$test_root/fault-unused.json" \
      >"$test_root/${profile}-${point}-client.log" 2>&1
    client_rc=$?
    wait "$server_pid"
    server_rc=$?
    set -e
    server_pid=
    [[ "$client_rc" -ne 0 ]] || fail "$profile $point crash client succeeded"
    [[ "$server_rc" -eq "$expected_server_rc" ]] ||
      fail "$profile $point server rc=$server_rc expected=$expected_server_rc"

    start_server
    recovery_report="$test_root/${profile}-${point}-recovery.json"
    java -cp "$classes_dir:$slf4j_jar" NativeLiteTpccTransactions \
      "$jdbc_url" "verify-crash-${profile}-${point}" "$recovery_report"
    grep -q '"atomicity":"pass"' "$recovery_report" ||
      fail "$profile $point recovery was not atomic"
    stop_server
  done
done

echo "Lite M14D isolation and crash-recovery checks passed"
