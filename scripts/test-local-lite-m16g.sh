#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -n "$artifact_dir" ]] || fail "usage: $0 ARTIFACT_DIR"
report="$artifact_dir/m16-report.json"
[[ -s "$report" ]] || fail "missing M16F report"

grep -q '"milestone":"M16F"' "$report" ||
  fail "M16 report milestone is missing"
grep -q '"full_scans":0' "$report" ||
  fail "M16 report does not prove zero Stock-Level full scans"
grep -q '"formal_tpmc_claim":"not_claimed"' "$report" ||
  fail "M16 report lost the formal claim boundary"

for document in \
  "$repo_root/README.md" \
  "$repo_root/benchmarks/tpcc/README.md" \
  "$repo_root/plan/local-lite.md" \
  "$repo_root/plan/local-lite-transaction-roadmap.md" \
  "$repo_root/plan/local-lite-legacy-regress-roadmap.md"; do
  grep -q 'M16' "$document" || fail "M16 is missing from $document"
done

echo "LocalLite M16G final evidence and documentation checks passed"
