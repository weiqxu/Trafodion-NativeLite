#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
asset_dir="$repo_root/benchmarks/tpcc"
properties="$asset_dir/m15-production.properties"
metrics="$asset_dir/occ-metrics.tsv"
artifact_dir=${1:-}

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -n "$artifact_dir" ]] || fail "usage: $0 ARTIFACT_DIR"
workload_report="$artifact_dir/workload-report.json"
operations_report="$artifact_dir/operations-report.json"
[[ -s "$workload_report" ]] || fail "missing M15G workload report"
[[ -s "$operations_report" ]] || fail "missing M15G operations report"

for expected in \
  '"contract_version":2' \
  '"isolation_model":"trafodion_mvcc_occ"' \
  '"warehouses":32' \
  '"terminals":32' \
  '"transaction_admission":"none_concurrent_occ"' \
  '"unclassified_errors":0' \
  '"consistency":"pass"'; do
  grep -q "$expected" "$workload_report" ||
    fail "workload report is missing $expected"
done

while IFS=$'\t' read -r metric _ required; do
  [[ "$metric" == "metric" || "$required" != "yes" ]] && continue
  grep -q "\"${metric}\":" "$workload_report" ||
    fail "workload report is missing OCC metric: $metric"
done <"$metrics"
grep -Eq '"transactions_started":[1-9][0-9]*' "$workload_report" ||
  fail "server observed no transactions"
grep -Eq '"transactions_committed":[1-9][0-9]*' "$workload_report" ||
  fail "server observed no commits"
for profile in new_order payment order_status delivery stock_level; do
  grep -Eq "\"${profile}\":\{\"committed\":[1-9][0-9]*" \
    "$workload_report" || fail "profile has no committed sample: $profile"
done

for operation in online_checkpoint clean_restart unclean_restart \
  checkpoint_restore disk_watermark; do
  grep -q "\"${operation}\":\"pass\"" "$operations_report" ||
    fail "operation did not pass: $operation"
done

property() {
  awk -F '=' -v key="$1" '$1 == key {print substr($0, index($0, "=") + 1)}' \
    "$properties"
}

workload=$(tr -d '\n' <"$workload_report")
operations=$(tr -d '\n' <"$operations_report")
schema_sha=$(sha256sum "$asset_dir/schema.sql" | awk '{print $1}')
driver_sha=$(sha256sum "$repo_root/scripts/NativeLiteTpccTransactions.java" | \
  awk '{print $1}')
config_sha=$(sha256sum "$properties" | awk '{print $1}')
source_revision=$(git -C "$repo_root" rev-parse HEAD)
report="$artifact_dir/qualification-report.json"

printf '{"contract_version":2,"milestone":"M15","source_revision":"%s","specification":{"name":"%s","version":"%s","claim":"tpc-c-like"},"revisions":{"schema_sha256":"%s","driver_sha256":"%s","configuration_sha256":"%s"},"scale":{"warehouses":32,"terminals":32},"isolation":{"level":"serializable","mechanism":"trafodion_mvcc_occ","client_writer_admission":"none","matrix":"pass","durable_decision_crash_cases":6},"claims":{"functional_support":"pass","repeatable_tpc_c_like":"pass","formal_tpc_c_compliance":"fail","official_tpmc":"not_claimed"},"workload":%s,"operations":%s}\n' \
  "$source_revision" "$(property specification.name)" \
  "$(property specification.version)" "$schema_sha" "$driver_sha" \
  "$config_sha" "$workload" "$operations" >"$report"

grep -q '"formal_tpc_c_compliance":"fail"' "$report" ||
  fail "aggregate report lost the formal-compliance boundary"
cat "$report"
echo "LocalLite M15 Release OCC qualification and claim checks passed"
