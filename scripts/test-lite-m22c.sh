#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/core/sql/litestore/LiteRocksDBStore.cpp"

grep -q 'LiteCommitCoordinator' "$source_file" || {
  echo "FAIL: M22C commit-intent coordinator is missing" >&2
  exit 1
}
grep -q 'maximum_physical_commit_overlap' "$source_file" || {
  echo "FAIL: M22C physical overlap metric is missing" >&2
  exit 1
}

"$repo_root/scripts/test-lite-occ-validation.sh"
"$repo_root/scripts/test-lite-sql-commit-recovery.sh"
echo "Lite M22C concurrent publication and recovery gate passed"
