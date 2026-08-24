#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server_source="$repo_root/core/sql/bin/TrafodionLiteServerMain.cpp"

grep -q 'maximumCompilerRequests_' "$server_source" || {
  echo "FAIL: compiler overlap metric is missing" >&2
  exit 1
}
if grep -q 'Trafodion Lite could not expand prepared predicate' "$server_source"; then
  echo "FAIL: prepared keyed predicates still use literal specialization" >&2
  exit 1
fi
grep -q 'boundKeyInputCount_' \
  "$repo_root/core/sql/executor/LiteStorageStubs.cpp" || {
  echo "FAIL: executor bound-key plan support is missing" >&2
  exit 1
}

"$repo_root/scripts/test-lite-t4jdbc.sh"
"$repo_root/scripts/test-lite-m21-concurrency.sh"
echo "Lite M22E bound-key and concurrent compiler gate passed"
