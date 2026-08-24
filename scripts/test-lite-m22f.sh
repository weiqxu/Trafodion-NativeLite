#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
properties="$repo_root/benchmarks/tpcc/m22-full.properties"
performance="$repo_root/scripts/test-lite-tpcc-performance.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }
property() { sed -n "s/^$1=//p" "$properties"; }
require_property() {
  [[ "$(property "$1")" == "$2" ]] ||
    fail "$1 must remain $2 for M22 qualification"
}
require_min_property() {
  local value
  value=$(property "$1")
  [[ "$value" =~ ^[0-9]+$ && "$value" -ge "$2" ]] ||
    fail "$1 must remain at least $2 for M22 qualification"
}

require_property warehouses 10
require_property districts.per.warehouse 10
require_property customers.per.district 3000
require_property orders.per.district 3000
require_property items 100000
require_property performance.warehouses 10
require_property performance.districts.per.warehouse 10
require_property performance.customers.per.district 3000
require_property performance.orders.per.district 3000
require_property performance.items 100000
require_property performance.terminals 32
require_min_property performance.warmup.transactions.per.terminal 20
require_min_property performance.measure.transactions.per.terminal 100
require_property performance.repetitions 3
require_property performance.min.throughput.tps 50
require_property performance.max.throughput.variance.ratio 0.10
require_property performance.max.p95.new_order.ms 1000
require_property performance.max.p95.payment.ms 500
require_property performance.max.p95.order_status.ms 500
require_property performance.max.p95.delivery.ms 2000
require_property performance.max.p95.stock_level.ms 2000

artifact_dir=${M22_REPORT_DIR:-/tmp/traf-lite-m22-full-report}
env TRAF_LITE_SYNC_COMMIT=1 \
  TPCC_PROPERTIES="$properties" TPCC_SCALE=qualification \
  TPCC_NATIVE_BULK_LOAD=1 TPCC_NATIVE_COMMIT_ROWS=10000 \
  TRAFODION_LITE_WORKERS=40 TPCC_ARTIFACT_DIR="$artifact_dir" \
  "$performance"

grep -q '"throughput_tps"' "$artifact_dir/workload-report.json" ||
  fail "workload throughput evidence is missing"
grep -q '"synchronous_commit":true' "$artifact_dir/workload-report.json" ||
  fail "qualification did not report synchronous commit"
grep -q '"unclassified_errors":0' "$artifact_dir/workload-report.json" ||
  fail "qualification reported unclassified errors"
grep -q '"consistency":"pass"' "$artifact_dir/workload-report.json" ||
  fail "qualification consistency evidence is missing"
loader_ms=$(sed -n 's/.*"elapsed_ms":\([0-9][0-9]*\).*/\1/p' \
  "$artifact_dir/bulk-load.json")
[[ -n "$loader_ms" && "$loader_ms" -le 3600000 ]] ||
  fail "full-cardinality loader exceeded the one-hour gate: ${loader_ms:-missing} ms"
echo "Lite M22F full-cardinality qualification passed"
