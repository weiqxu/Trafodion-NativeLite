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

phase_case() {
  local phase=$1
  local source=$2
  local pattern=$3
  local description=$4
  grep -Fq "$pattern" "$source" ||
    fail "$phase gate missing: $description"
  echo "$phase PASS: $description"
}

# Keep the six M10 sub-phases explicit.  The legacy adapter supplies the
# portable-suite evidence where a legacy body is safe; the native lane supplies
# the focused RocksDB executor coverage for the remaining phases.  A phase is
# never considered green merely because the test was skipped.
phase_case M10A "$legacy_output" 'PASS executor/TEST014[all]' \
  'CTAS/volatile/physical-attribute DDL on RocksDB tables'
phase_case M10A "$native_output" 'PASS TEST009' \
  'native catalog/view/ALTER/TRUNCATE DDL'
phase_case M10B "$native_output" 'PASS TEST038' \
  'RocksDB statistics refresh and UEC'
phase_case M10C "$legacy_output" 'PASS core/TEST018[all]' \
  'secondary-index INSERT/UPDATE/DELETE/rollback maintenance'
for test_number in 026 027 028 029 030 031 032 033 034 035; do
  phase_case M10C "$native_output" "PASS TEST$test_number" \
    "native DML/index coverage TEST$test_number"
done
phase_case M10D "$legacy_output" 'PASS charsets/TEST003[all]' \
  'UCS2 assignment and DML compatibility'
phase_case M10D "$legacy_output" 'PASS charsets/TEST316[all]' \
  'character-set translation compatibility'
phase_case M10D "$native_output" 'PASS TEST039' \
  'native character-set and data-type storage'
phase_case M10E "$native_output" 'PASS TEST040' \
  'cursor/window/grouping advanced executor coverage'
phase_case M10F "$native_output" 'PASS TEST041' \
  'RocksDB catalog authorization and revocation'
phase_case M10F "$native_output" 'PASS TEST042' \
  'RocksDB-only UDR DDL/invocation lifecycle'

cat "$legacy_output"
cat "$native_output"
echo "local-lite legacy convergence gate passed"
