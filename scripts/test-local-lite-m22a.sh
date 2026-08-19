#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
store_source="$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp"
plan="$repo_root/plan/local-lite-m22-concurrency-qualification.md"

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -s "$plan" ]] || fail "missing M22 plan"

for field in commit_latch_wait_us validation_latency_us \
  referential_validation_latency_us batch_build_latency_us \
  publication_latency_us visibility_publication_latency_us \
  point_validation_conflicts range_validation_conflicts \
  full_scan_validation_conflicts; do
  grep -q "$field" "$store_source" ||
    fail "missing M22A metric: $field"
done

grep -q 'M22 remains active if any prerequisite' "$plan" ||
  fail "M22 completion boundary is missing"
echo "LocalLite M22A stage and conflict telemetry contract passed"
