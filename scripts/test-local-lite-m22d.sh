#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
loader_source="$repo_root/core/sql/bin/NativeLiteBulkLoaderMain.cpp"

grep -q 'ResumeManifest' "$loader_source" || {
  echo "FAIL: resumable loader manifest is missing" >&2
  exit 1
}
grep -q 'maximum_physical_commit_overlap' \
  "$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp" || {
  echo "FAIL: parallel physical commit proof is missing" >&2
  exit 1
}
if grep -q 'gBulkPublicationMutex' "$loader_source"; then
  echo "FAIL: loader still serializes all physical publications" >&2
  exit 1
fi

"$repo_root/scripts/test-local-lite-m22c.sh"
echo "LocalLite M22D resumable parallel loader contract passed"
