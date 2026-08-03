#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
legacy_runner="$repo_root/core/sql/regress/localLiteLegacy/runregr"
native_runner="$repo_root/core/sql/regress/localLite/runregr"
audit="$repo_root/scripts/audit-local-lite-legacy-regress.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$legacy_runner" ]] || fail "missing legacy runner: $legacy_runner"
[[ -x "$native_runner" ]] || fail "missing native runner: $native_runner"
[[ -x "$audit" ]] || fail "missing legacy manifest audit: $audit"

"$audit" --check >/dev/null || fail "legacy manifest audit failed"

legacy_output=$(mktemp /tmp/traf-local-lite-m10-legacy.XXXXXX)
native_output=$(mktemp /tmp/traf-local-lite-m10-native.XXXXXX)
trap 'rm -f "$legacy_output" "$native_output"' EXIT

"$legacy_runner" >"$legacy_output"
grep -Eq 'Summary: [1-9][0-9]* passed, 0 failed, 0 skipped' "$legacy_output" ||
  fail "legacy runnable allowlist did not converge"

"$native_runner" >"$native_output"
grep -q 'Summary: 42 passed, 0 failed' "$native_output" ||
  fail "native local-lite suite regressed"

cat "$legacy_output"
cat "$native_output"
echo "local-lite legacy convergence gate passed"
