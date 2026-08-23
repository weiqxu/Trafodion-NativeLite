#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/nativelite-server"
client="$repo_root/core/sqf/export/bin64d/nativelite-client"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$server" ]] || fail "missing built NativeLite server: $server"
[[ -x "$client" ]] || fail "missing built NativeLite client: $client"

test_root=$(mktemp -d /tmp/traf-lite-client.XXXXXX)
store_dir="$test_root/store"
server_log="$test_root/server.log"
mkdir -p "$store_dir"
server_pid=
port=${NATIVELITE_TEST_PORT:-$((25000 + ($$ % 10000)))}

cleanup() {
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

env TRAF_HOME="$traf_home" TRAF_LITE=1 \
  TRAF_LITE_STORE_DIR="$store_dir" \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --listen 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  if grep -q 'NativeLite server ready' "$server_log"; then break; fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$server_log" >&2
    fail "NativeLite server exited during client startup"
  fi
  sleep 0.05
done
grep -q 'NativeLite server ready' "$server_log" || fail "NativeLite server did not become ready"

output=$(printf '%s\n' \
  'create table client_cli_test (id int not null, name varchar(20));' \
  "insert into client_cli_test values (1, 'alpha');" \
  "insert into client_cli_test values (2, 'beta');" \
  'select id, name from client_cli_test order by id;' \
  'get tables;' \
  'get schemas;' \
  'drop table client_cli_test;' \
  'exit;' | "$client" --host 127.0.0.1 --port "$port")

grep -q 'alpha' <<<"$output" || fail "client output did not contain alpha"
grep -q 'beta' <<<"$output" || fail "client output did not contain beta"
grep -q '2 row(s) selected' <<<"$output" || fail "client did not report two selected rows"
grep -q 'TABLE_NAME' <<<"$output" || fail "client did not return GET TABLES columns"
grep -q 'CLIENT_CLI_TEST' <<<"$output" || fail "client did not return GET TABLES rows"
grep -q 'SCHEMA_NAME' <<<"$output" || fail "client did not return GET SCHEMAS columns"
grep -q 'SEABASE' <<<"$output" || fail "client did not return GET SCHEMAS rows"
grep -q 'SQL operation complete' <<<"$output" || fail "client did not report DDL completion"

echo "Lite Storage SQLCI-style client checks passed"
