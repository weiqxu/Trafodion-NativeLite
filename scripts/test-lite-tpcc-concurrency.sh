#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/trafodion-lite-server"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
loader_source="$repo_root/scripts/TrafodionLiteTpcc.java"
concurrency_source="$repo_root/scripts/TrafodionLiteTpccConcurrency.java"
inventory="$repo_root/benchmarks/tpcc/m14e-runtime-inventory.tsv"
properties="$repo_root/benchmarks/tpcc/qualification.properties"
schema="$repo_root/benchmarks/tpcc/schema.sql"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$server" ]] || fail "missing built Trafodion Lite server: $server"
[[ $(wc -l <"$inventory") -eq 16 ]] || fail "M14E runtime inventory drifted"
grep -q $'default catalog and schema\tprocess environment' "$inventory" ||
  fail "M14E runtime inventory is missing schema ownership"

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

test_root=$(mktemp -d /tmp/traf-lite-m14e.XXXXXX)
store_dir="$test_root/store"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
report="$test_root/concurrency-report.json"
mkdir -p "$store_dir" "$classes_dir"
server_pid=
port=${TRAFODION_LITE_TEST_PORT:-$((28000 + ($$ % 9000)))}
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"

cleanup() {
  rc=$?
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" -ne 0 && -s "$server_log" ]]; then
    sed -n '1,320p' "$server_log" >&2
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
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
server_pid=$!
for _ in $(seq 1 200); do
  if grep -q 'Trafodion Lite server ready' "$server_log"; then break; fi
  kill -0 "$server_pid" 2>/dev/null || fail "M14E server exited during startup"
  sleep 0.05
done
grep -q 'Trafodion Lite server ready' "$server_log" || fail "M14E server not ready"

java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpcc \
  "$jdbc_url" load smoke "$properties" "$schema" "$test_root/load.json"
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpccConcurrency \
  "$jdbc_url" "$report"
grep -q '"compiler_executor_overlap":"pass"' "$report" ||
  fail "M14E executor overlap was not observed"

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
echo "Lite M14E concurrent compiler/executor checks passed"
