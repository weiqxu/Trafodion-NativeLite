#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
evidence="$repo_root/benchmarks/tpcc/m22-qualification-evidence.json"
properties="$repo_root/benchmarks/tpcc/m22-full.properties"
makefile="$repo_root/Makefile"
readme="$repo_root/README.md"
roadmap="$repo_root/plan/lite-transaction-roadmap.md"
design="$repo_root/plan/lite-m22-concurrency-qualification.md"

fail() { echo "FAIL: $*" >&2; exit 1; }

for target in 18 19 20 21 22; do
  grep -q "^lite-m${target}:" "$makefile" ||
    fail "Makefile is missing aggregate target lite-m${target}"
done
for phase in a b c d e f g h; do
  grep -q "^lite-m22${phase}:" "$makefile" ||
    fail "Makefile is missing M22${phase^^} target"
done

grep -q 'M22 full-cardinality qualification is complete' "$readme" ||
  fail "README M22 status is stale"
grep -q '## Milestone 22: Concurrent commit and full-scale qualification' \
  "$roadmap" || fail "transaction roadmap is missing M22"
grep -Eq 'Status: (M22A-M22G complete; M22H release audit active\.|complete\.)' \
  "$design" ||
  fail "M22 phase status is not synchronized"

config_hash=$(sha256sum "$properties" | awk '{print $1}')
grep -q "\"configuration_sha256\": \"$config_hash\"" "$evidence" ||
  fail "compact evidence configuration checksum is stale"
grep -q '"throughput_tps": 54.304' "$evidence" ||
  fail "compact evidence is missing the qualified throughput"
grep -q '"throughput_variance_ratio": 0.021806' "$evidence" ||
  fail "compact evidence is missing the qualified variance"

if [[ -n "$artifact_dir" ]]; then
  [[ -d "$artifact_dir" ]] || fail "artifact directory does not exist: $artifact_dir"
  for artifact in bulk-load.json bulk-load.manifest load.json \
      operations-report.json qualification.properties workload-report.json; do
    [[ -s "$artifact_dir/$artifact" ]] || fail "missing artifact: $artifact"
    digest=$(sha256sum "$artifact_dir/$artifact" | awk '{print $1}')
    grep -q "\"$artifact\": \"$digest\"" "$evidence" ||
      fail "artifact checksum mismatch: $artifact"
  done
fi

echo "Lite M22G aggregate target and evidence consistency checks passed"
