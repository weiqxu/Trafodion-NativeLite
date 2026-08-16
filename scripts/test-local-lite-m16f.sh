#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${TPCC_ARTIFACT_DIR:-/tmp/traf-local-lite-m16-report}
performance_script="$repo_root/scripts/test-local-lite-tpcc-performance.sh"
workload_report="$artifact_dir/workload-report.json"

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -x "$performance_script" ]] || fail "missing M15 performance runner"
mkdir -p "$artifact_dir"

TPCC_ARTIFACT_DIR="$artifact_dir" "$performance_script"
[[ -s "$workload_report" ]] || fail "M16F workload report was not produced"

grep -Eq '"stock_level_access":\{"range_scans":[1-9][0-9]*' \
  "$workload_report" || fail "Stock-Level recorded no range scans"
grep -Eq '"point_reads":[1-9][0-9]*' "$workload_report" ||
  fail "Stock-Level recorded no stock point reads"
grep -Eq '"batch_reads":[1-9][0-9]*' "$workload_report" ||
  fail "Stock-Level recorded no batch reads"
grep -Eq '"full_scans":0' "$workload_report" ||
  fail "Stock-Level full-scan policy was not satisfied"
for profile in new_order payment order_status delivery stock_level; do
  grep -Eq "\"${profile}\":\{\"committed\":[1-9][0-9]*" \
    "$workload_report" || fail "profile has no committed sample: $profile"
done

throughput=$(sed -n 's/.*"throughput_tps":\([^,]*\).*/\1/p' \
  "$workload_report" | head -1)
stock_p95=$(sed -n 's/.*"stock_level"[^}]*"p95":\([0-9]*\).*/\1/p' \
  "$workload_report" | head -1)
batch_reads=$(sed -n 's/.*"batch_reads":\([0-9]*\).*/\1/p' \
  "$workload_report" | head -1)
printf '{"contract_version":1,"milestone":"M16F","source_revision":"%s","stock_level_access":{"range_scans":"required_nonzero","point_reads":"required_nonzero","batch_reads":%s,"full_scans":0},"throughput_tps":%s,"stock_level_p95_us":%s,"production_target":{"throughput_tps":50,"stock_level_p95_ms":2000},"formal_tpmc_claim":"not_claimed"}\n' \
  "$(git -C "$repo_root" rev-parse HEAD)" "${batch_reads:-0}" \
  "${throughput:-0}" "${stock_p95:-0}" \
  >"$artifact_dir/m16-report.json"
cat "$artifact_dir/m16-report.json"
echo "LocalLite M16F Release Stock-Level qualification passed"
