#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/trafodion-lite-server"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
loader_source="$repo_root/scripts/TrafodionLiteTpcc.java"
concurrency_source="$repo_root/scripts/TrafodionLiteTpccConcurrency.java"
properties="$repo_root/benchmarks/tpcc/qualification.properties"
schema="$repo_root/benchmarks/tpcc/schema.sql"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"
clients=${TRAFODION_LITE_M21_CLIENTS:-32}

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$server" ]] || fail "missing built Trafodion Lite server: $server"
[[ "$clients" =~ ^[0-9]+$ && "$clients" -ge 2 && "$clients" -le 256 ]] ||
  fail "TRAFODION_LITE_M21_CLIENTS must be between 2 and 256"

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

test_root=$(mktemp -d /tmp/traf-lite-m21.XXXXXX)
store_dir="$test_root/store"
capacity_store_dir="$test_root/capacity-store"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
capacity_log="$test_root/capacity-server.log"
report="$test_root/concurrency-report.json"
mkdir -p "$store_dir" "$capacity_store_dir" "$classes_dir"
server_pid=
capacity_pid=
port=${TRAFODION_LITE_TEST_PORT:-$((30000 + ($$ % 7000)))}
capacity_port=$((port + 1))
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"
capacity_url="jdbc:t4jdbc://127.0.0.1:${capacity_port}/:"

cleanup() {
  rc=$?
  for pid in "${server_pid:-}" "${capacity_pid:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [[ "$rc" -ne 0 ]]; then
    [[ -s "$server_log" ]] && sed -n '1,320p' "$server_log" >&2
    [[ -s "$capacity_log" ]] && sed -n '1,160p' "$capacity_log" >&2
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

find "$driver_source" -name '*.java' -print0 | \
  xargs -0 javac -nowarn -cp "$slf4j_jar" -d "$classes_dir"
javac -Xlint:all -cp "$classes_dir:$slf4j_jar" -d "$classes_dir" \
  "$loader_source" "$concurrency_source"

env TRAF_HOME="$traf_home" TRAF_LITE=1 \
  TRAF_LITE_STORE_DIR="$store_dir" TRAF_LITE_EXECUTOR_HOLD_MS=400 \
  TRAF_LITE_COMPILER_HOLD_MS=100 \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --listen 127.0.0.1 --port "$port" --workers "$clients" >"$server_log" 2>&1 &
server_pid=$!
for _ in $(seq 1 200); do
  if grep -q 'Trafodion Lite server ready' "$server_log"; then break; fi
  kill -0 "$server_pid" 2>/dev/null || fail "M21 server exited during startup"
  sleep 0.05
done
grep -q 'Trafodion Lite server ready' "$server_log" || fail "M21 server not ready"
grep -q "workers=$clients" "$server_log" || fail "M21 worker limit was not applied"

java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpcc \
  "$jdbc_url" load smoke "$properties" "$schema" "$test_root/load.json"
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpccConcurrency \
  "$jdbc_url" "$report" "$clients"
grep -q '"client_count":'"$clients" "$report" || fail "M21 client count missing from report"

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=

env TRAF_HOME="$traf_home" TRAF_LITE=1 \
  TRAF_LITE_STORE_DIR="$capacity_store_dir" \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --listen 127.0.0.1 --port "$capacity_port" --workers 2 >"$capacity_log" 2>&1 &
capacity_pid=$!
for _ in $(seq 1 200); do
  if grep -q 'Trafodion Lite server ready' "$capacity_log"; then break; fi
  kill -0 "$capacity_pid" 2>/dev/null || fail "M21 capacity server exited during startup"
  sleep 0.05
done
grep -q 'Trafodion Lite server ready' "$capacity_log" || fail "M21 capacity server not ready"
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpccConcurrency "$capacity_url" capacity

kill -TERM "$capacity_pid"
wait "$capacity_pid"
capacity_pid=
echo "Lite M21 $clients-worker session isolation and capacity checks passed"
