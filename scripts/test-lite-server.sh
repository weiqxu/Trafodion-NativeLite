#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server="$repo_root/core/sqf/export/bin64d/nativelite-server"
sql_libs="$repo_root/core/sql/lib/linux/64bit/debug"
sqf_libs="$repo_root/core/sqf/export/lib64d"
traf_home="$repo_root/core/sqf"

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$server" ]] || fail "missing built NativeLite server: $server"

test_root=$(mktemp -d /tmp/traf-lite-server.XXXXXX)
store_dir="$test_root/store"
socket_path="$test_root/nativelite.t4.sock"
server_log="$test_root/server.log"
mkdir -p "$store_dir"
server_pid=

cleanup() {
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

start_unix() {
  : >"$server_log"
  env TRAF_HOME="$traf_home" TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
    "$server" --unix-socket "$socket_path" >"$server_log" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 100); do
    if grep -q 'NativeLite server ready' "$server_log"; then
      [[ -S "$socket_path" ]] || fail "ready server has no Unix socket"
      return
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      cat "$server_log" >&2
      fail "NativeLite server exited during startup"
    fi
    sleep 0.05
  done
  fail "NativeLite Unix socket was not created"
}

start_unix
[[ "$(stat -c '%a' "$socket_path")" == "600" ]] ||
  fail "Unix socket permissions are not 0600"

contender_log="$test_root/contender.log"
set +e
env TRAF_HOME="$traf_home" TRAF_LITE=1 \
  TRAF_LITE_STORE_DIR="$store_dir" \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --unix-socket "$test_root/contender.sock" \
  >"$contender_log" 2>&1
contender_rc=$?
set -e
[[ "$contender_rc" -ne 0 ]] || fail "second server opened the same store"
grep -Eqi 'lock|open RocksDB catalog' "$contender_log" || {
  cat "$contender_log" >&2
  fail "second-server failure did not identify store ownership"
}

kill -KILL "$server_pid"
set +e
wait "$server_pid"
set -e
server_pid=
start_unix
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
[[ ! -e "$socket_path" ]] || fail "graceful shutdown left its Unix socket"

start_unix
rm "$socket_path"
replacement_content=replacement-must-survive
printf '%s\n' "$replacement_content" >"$socket_path"
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
[[ -f "$socket_path" ]] ||
  fail "shutdown removed a pathname that replaced the server socket"
[[ "$(sed -n '1p' "$socket_path")" == "$replacement_content" ]] ||
  fail "shutdown changed the replacement pathname"

regular_path="$test_root/regular"
touch "$regular_path"
set +e
env TRAF_HOME="$traf_home" TRAF_LITE=1 \
  TRAF_LITE_STORE_DIR="$test_root/other-store" \
  LD_LIBRARY_PATH="$sql_libs:$sqf_libs:${LD_LIBRARY_PATH:-}" \
  "$server" --unix-socket "$regular_path" >"$server_log" 2>&1
regular_rc=$?
set -e
[[ "$regular_rc" -ne 0 && -f "$regular_path" ]] ||
  fail "server replaced an existing non-socket path"

echo "Lite Storage standalone server lifecycle checks passed"
