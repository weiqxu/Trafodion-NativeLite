#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
report_dir=${1:-/tmp/traf-lite-m22-release-audit}
full_report_dir="$report_dir/full-qualification"
audit_log="$report_dir/release-audit.log"
audit_json="$report_dir/release-audit.json"

fail() { echo "FAIL: $*" >&2; exit 1; }
require_clean() {
  [[ -z "$(git -C "$repo_root" status --porcelain --untracked-files=all)" ]] ||
    fail "M22H requires a clean source tree"
}
run_gate() {
  local name=$1
  shift
  echo "M22H gate=$name status=started" | tee -a "$audit_log"
  "$@" 2>&1 | tee -a "$audit_log"
  echo "M22H gate=$name status=passed" | tee -a "$audit_log"
}

require_clean
revision=$(git -C "$repo_root" rev-parse HEAD)
mkdir -p "$report_dir"
: >"$audit_log"

run_gate release-build make -C "$repo_root" lite-release -j2
run_gate debug-build make -C "$repo_root" lite -j2

# M10 includes all 43 native cases and the complete 11-case portable legacy
# allowlist. Later milestone commands retain their authoritative focused gates
# while avoiding obsolete reduced performance reports superseded by M22F.
run_gate m10-native-legacy env LITE_LEGACY_TIMEOUT=300 \
  make -C "$repo_root" lite-m10
run_gate m11-runtime make -C "$repo_root" lite-m11
run_gate m12-storage make -C "$repo_root" lite-m12
run_gate m13-cutover "$repo_root/scripts/test-lite-storage-cutover.sh"
run_gate m14-tpcc-baseline "$repo_root/scripts/test-lite-tpcc-baseline.sh"
run_gate m14-tpcc-loader "$repo_root/scripts/test-lite-tpcc-loader.sh"
run_gate m14-tpcc-transactions \
  "$repo_root/scripts/test-lite-tpcc-transactions.sh"
run_gate m14-isolation-recovery \
  "$repo_root/scripts/test-lite-tpcc-isolation.sh"
run_gate m14-concurrency "$repo_root/scripts/test-lite-tpcc-concurrency.sh"
run_gate m15-occ-contract "$repo_root/scripts/test-lite-occ-contract.sh"
run_gate m15-statement-snapshot \
  "$repo_root/scripts/test-lite-statement-snapshot.sh"
run_gate m15-transaction-snapshot \
  "$repo_root/scripts/test-lite-transaction-snapshot.sh"
run_gate m15-occ-validation "$repo_root/scripts/test-lite-occ-validation.sh"
run_gate m15-fault-recovery \
  "$repo_root/scripts/test-lite-sql-commit-recovery.sh"
run_gate m16-stock-index "$repo_root/scripts/test-lite-m16b.sh"
run_gate m16-stock-source "$repo_root/scripts/test-lite-m16c.sh"
run_gate m16-stock-plan "$repo_root/scripts/test-lite-m16d.sh"
run_gate m16-stock-contract "$repo_root/scripts/test-lite-m16e.sh"
run_gate m17-source "$repo_root/scripts/test-lite-m17.sh"
run_gate m17-new-order "$repo_root/scripts/test-lite-new-order-batch.sh"
run_gate m18-t4 "$repo_root/scripts/test-lite-t4jdbc.sh"
run_gate m19-storage "$repo_root/scripts/test-lite-storage-contract.sh"
run_gate m20-five-profile \
  "$repo_root/scripts/test-lite-tpcc-transactions.sh"
run_gate m21-workers "$repo_root/scripts/test-lite-m21-concurrency.sh"

for phase in a b c d e; do
  run_gate "m22${phase}" "$repo_root/scripts/test-lite-m22${phase}.sh"
done
run_gate m22g "$repo_root/scripts/test-lite-m22g.sh"
run_gate release-qualification-build \
  make -C "$repo_root" lite-release -j2
run_gate m22f-full env TRAF_LITE_SYNC_COMMIT=1 \
  LITE_BUILD_TYPE=release M22_REPORT_DIR="$full_report_dir" \
  "$repo_root/scripts/test-lite-m22f.sh"

grep -q "\"source_revision\":\"$revision\"" \
  "$full_report_dir/operations-report.json" ||
  fail "full qualification does not identify audited revision $revision"
grep -q '"throughput_tps"' "$full_report_dir/workload-report.json" ||
  fail "full qualification throughput is missing"
grep -q '"synchronous_commit":true' "$full_report_dir/workload-report.json" ||
  fail "full qualification did not use synchronous commit"
grep -q '"consistency":"pass"' "$full_report_dir/workload-report.json" ||
  fail "full qualification consistency failed"

require_clean
workload_sha=$(sha256sum "$full_report_dir/workload-report.json" | awk '{print $1}')
operations_sha=$(sha256sum "$full_report_dir/operations-report.json" | awk '{print $1}')
printf '{"contract_version":1,"milestone":"M22H",' >"$audit_json"
printf '"source_revision":"%s","source_tree":"clean",' "$revision" >>"$audit_json"
printf '"debug_build":"pass","release_build":"pass",' >>"$audit_json"
printf '"m10_m22_gates":"pass","native_regress":"pass",' >>"$audit_json"
printf '"legacy_allowlist":"pass","t4":"pass","occ_isolation":"pass",' >>"$audit_json"
printf '"fault_recovery":"pass","full_qualification":"pass",' >>"$audit_json"
printf '"workload_sha256":"%s","operations_sha256":"%s"}\n' \
  "$workload_sha" "$operations_sha" >>"$audit_json"

echo "Lite M22H release audit passed for $revision"
