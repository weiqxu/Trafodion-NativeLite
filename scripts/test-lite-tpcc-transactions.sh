#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/trafodion-lite-server"
driver_source="$repo_root/core/conn/jdbcT4/src/main/java"
loader_source="$repo_root/scripts/TrafodionLiteTpcc.java"
transaction_source="$repo_root/scripts/TrafodionLiteTpccTransactions.java"
properties="$repo_root/benchmarks/tpcc/qualification.properties"
schema="$repo_root/benchmarks/tpcc/schema.sql"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$server" ]] || fail "missing built Trafodion Lite server: $server"
command -v javac >/dev/null || fail "javac is required for the M14C gate"
command -v java >/dev/null || fail "java is required for the M14C gate"

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

test_root=$(mktemp -d /tmp/traf-lite-m14c.XXXXXX)
store_dir="$test_root/store"
classes_dir="$test_root/classes"
server_log="$test_root/server.log"
load_report="$test_root/load-report.json"
transaction_report="$test_root/transaction-report.json"
verify_report="$test_root/verify-report.json"
mkdir -p "$store_dir" "$classes_dir"
server_pid=
port=${TRAFODION_LITE_TEST_PORT:-$((26000 + ($$ % 10000)))}
jdbc_url="jdbc:t4jdbc://127.0.0.1:${port}/:"

cleanup() {
  rc=$?
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" -ne 0 && -s "$server_log" ]]; then
    echo "Trafodion Lite M14C server log:" >&2
    sed -n '1,260p' "$server_log" >&2
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

start_server() {
  : >"$server_log"
  env TRAF_HOME="$traf_home" TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 200); do
    if grep -q 'Trafodion Lite server ready' "$server_log"; then return; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      cat "$server_log" >&2
      fail "Trafodion Lite server exited during M14C startup"
    fi
    sleep 0.05
  done
  fail "Trafodion Lite server did not become ready for M14C"
}

stop_server() {
  kill -TERM "$server_pid"
  set +e
  wait "$server_pid"
  rc=$?
  set -e
  server_pid=
  [[ "$rc" -eq 0 ]] || fail "Trafodion Lite graceful shutdown returned $rc"
}

find "$driver_source" -name '*.java' -print0 | \
  xargs -0 javac -nowarn -cp "$slf4j_jar" -d "$classes_dir"
javac -Xlint:all -cp "$classes_dir:$slf4j_jar" -d "$classes_dir" \
  "$loader_source" "$transaction_source"

start_server
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpcc \
  "$jdbc_url" load smoke "$properties" "$schema" "$load_report"
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpccTransactions \
  "$jdbc_url" run "$transaction_report"
grep -q '"unclassified_errors":0' "$transaction_report" ||
  fail "M14C transaction report contains errors"
grep -q '"consistency":"pass"' "$transaction_report" ||
  fail "M14C transaction report did not pass consistency"
stop_server

start_server
java -cp "$classes_dir:$slf4j_jar" TrafodionLiteTpccTransactions \
  "$jdbc_url" verify "$verify_report"
stop_server

echo "Lite M14C five-profile T4 JDBC transaction checks passed"
