#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp"

for token in '"point"' '"range"' '"full_scan"' \
  start_sequence write_sequence; do
  grep -q "$token" "$source_file" || {
    echo "FAIL: missing precise OCC diagnostic $token" >&2
    exit 1
  }
done

"$repo_root/scripts/test-local-lite-occ-validation.sh"
echo "LocalLite M22B precise OCC conflict gate passed"
