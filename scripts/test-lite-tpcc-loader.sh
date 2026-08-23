#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/nativelite-server"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
test_source="$repo_root/scripts/NativeLiteTpcc.java"
properties="$repo_root/benchmarks/tpcc/qualification.properties"
schema="$repo_root/benchmarks/tpcc/schema.sql"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"
scale=${TPCC_M14_SCALE:-smoke}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$server" ]] || fail "missing built NativeLite server: $server"
[[ -f "$test_source" ]] || fail "missing TPC-C loader: $test_source"
[[ -f "$schema" ]] || fail "missing TPC-C schema: $schema"
command -v javac >/dev/null || fail "javac is required for the M14B gate"
command -v java >/dev/null || fail "java is required for the M14B gate"

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

test_root=$(mktemp -d /tmp/traf-lite-m14b.XXXXXX)
store_dir="$test_root/store"
restored_dir="$test_root/restored"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
report="$test_root/report.json"
mkdir -p "$store_dir" "$classes_dir"
server_pid=
port=${NATIVELITE_TEST_PORT:-$((24000 + ($$ % 12000)))}
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"

cleanup() {
  rc=$?
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" -ne 0 && -s "$server_log" ]]; then
    echo "NativeLite M14B server log:" >&2
    sed -n '1,260p' "$server_log" >&2
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

start_server() {
  local selected_store=$1
  : >"$server_log"
  env TRAF_HOME="$traf_home" TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$selected_store" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 200); do
    if grep -q 'NativeLite server ready' "$server_log"; then return; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      cat "$server_log" >&2
      fail "NativeLite server exited during M14B startup"
    fi
    sleep 0.05
  done
  fail "NativeLite server did not become ready for M14B"
}

stop_server() {
  kill -TERM "$server_pid"
  set +e
  wait "$server_pid"
  rc=$?
  set -e
  server_pid=
  [[ "$rc" -eq 0 ]] || fail "NativeLite graceful shutdown returned $rc"
}

find "$driver_source" -name '*.java' -print0 | \
  xargs -0 javac -nowarn -cp "$slf4j_jar" -d "$classes_dir"
javac -Xlint:all -cp "$classes_dir:$slf4j_jar" -d "$classes_dir" "$test_source"

start_server "$store_dir"
set +e
TPCC_FAIL_AFTER_BATCHES=5 java -cp "$classes_dir:$slf4j_jar" \
  NativeLiteTpcc "$jdbc_url" load "$scale" "$properties" "$schema" "$report" \
  >"$test_root/interrupted.out" 2>&1
interrupted_rc=$?
set -e
[[ "$interrupted_rc" -ne 0 ]] || fail "injected loader interruption succeeded"
grep -q 'injected loader interruption' "$test_root/interrupted.out" ||
  fail "loader interruption did not report the injected boundary"

java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
  "$jdbc_url" load "$scale" "$properties" "$schema" "$report"
grep -q '"consistency":"pass"' "$report" || fail "load report is not passing"
stop_server

cp -a "$store_dir" "$restored_dir"
start_server "$store_dir"
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
  "$jdbc_url" verify "$scale" "$properties" "$schema" "$report"
stop_server

start_server "$restored_dir"
java -cp "$classes_dir:$slf4j_jar" NativeLiteTpcc \
  "$jdbc_url" verify "$scale" "$properties" "$schema" "$report"
stop_server

echo "Lite M14B TPC-C schema, loader, restart, and restore checks passed ($scale)"
