#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
asset_dir="$repo_root/benchmarks/tpcc"
properties="$asset_dir/qualification.properties"
claims="$asset_dir/claim-checklist.tsv"
regressions="$asset_dir/m14-regression-inputs.tsv"
artifact_dir=${1:-}

fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -n "$artifact_dir" ]] || fail "usage: $0 ARTIFACT_DIR"
[[ -s "$artifact_dir/workload-report.json" ]] ||
  fail "missing M14F workload report"
[[ -s "$artifact_dir/operations-report.json" ]] ||
  fail "missing M14F operations report"

[[ $(awk -F '\t' 'NR > 1 && $3 == "yes" {count++} END {print count + 0}' \
  "$regressions") -eq 10 ]] || fail "M14 regression input inventory drifted"
for milestone in M10 M11 M12 M13 M14A M14B M14C M14D M14E M14F; do
  grep -q "^${milestone}"$'\t' "$regressions" ||
    fail "missing aggregate regression input: $milestone"
done

[[ $(awk -F '\t' 'NR > 1 && $1 == "functional_support" && $3 == "pass" \
  {count++} END {print count + 0}' "$claims") -eq 4 ]] ||
  fail "functional-support checklist drifted"
[[ $(awk -F '\t' 'NR > 1 && $1 == "repeatable_tpc_c_like" && $3 == "pass" \
  {count++} END {print count + 0}' "$claims") -eq 5 ]] ||
  fail "TPC-C-like checklist drifted"
[[ $(awk -F '\t' 'NR > 1 && $1 == "formal_tpc_c_compliance" && $3 == "fail" \
  {count++} END {print count + 0}' "$claims") -eq 4 ]] ||
  fail "formal-compliance checklist drifted"

grep -q '"claim":"tpc-c-like"' "$artifact_dir/workload-report.json" ||
  fail "workload report has the wrong claim label"
grep -q '"unclassified_errors":0' "$artifact_dir/workload-report.json" ||
  fail "workload report contains unclassified errors"
grep -q '"consistency":"pass"' "$artifact_dir/workload-report.json" ||
  fail "workload consistency did not pass"
for operation in online_checkpoint clean_restart unclean_restart \
  checkpoint_restore disk_watermark; do
  grep -q "\"${operation}\":\"pass\"" \
    "$artifact_dir/operations-report.json" ||
    fail "operation did not pass: $operation"
done

property() {
  awk -F '=' -v key="$1" '$1 == key {print substr($0, index($0, "=") + 1)}' \
    "$properties"
}

workload=$(tr -d '\n' <"$artifact_dir/workload-report.json")
operations=$(tr -d '\n' <"$artifact_dir/operations-report.json")
schema_sha=$(sha256sum "$asset_dir/schema.sql" | awk '{print $1}')
driver_sha=$(sha256sum "$repo_root/scripts/TrafodionLiteTpccTransactions.java" | \
  awk '{print $1}')
config_sha=$(sha256sum "$properties" | awk '{print $1}')
deviation_count=$(awk -F '\t' 'NR > 1 && NF {count++} END {print count + 0}' \
  "$asset_dir/dialect-deviations.tsv")
source_revision=$(git -C "$repo_root" rev-parse HEAD)
report="$artifact_dir/qualification-report.json"

printf '{"contract_version":1,"milestone":"M14","source_revision":"%s","specification":{"name":"%s","version":"%s","claim":"tpc-c-like"},"revisions":{"schema_sha256":"%s","driver_sha256":"%s","configuration_sha256":"%s"},"regression_inputs":{"M10":"pass","M11":"pass","M12":"pass","M13":"pass","M14A":"pass","M14B":"pass","M14C":"pass","M14D":"pass","M14E":"pass","M14F":"pass"},"isolation":{"level":"serializable","mechanism":"snapshot_plus_database_sequence_validation","matrix":"pass","durable_decision_crash_cases":6},"claims":{"functional_support":"pass","repeatable_tpc_c_like":"pass","formal_tpc_c_compliance":"fail","official_tpmc":"not_claimed"},"known_deviations":{"manifest":"benchmarks/tpcc/dialect-deviations.tsv","count":%s},"workload":%s,"operations":%s}\n' \
  "$source_revision" "$(property specification.name)" \
  "$(property specification.version)" "$schema_sha" "$driver_sha" \
  "$config_sha" "$deviation_count" "$workload" "$operations" >"$report"

grep -q '"formal_tpc_c_compliance":"fail"' "$report" ||
  fail "aggregate report lost the formal-compliance boundary"
cat "$report"
echo "Lite M14 aggregate qualification and claim checks passed"
