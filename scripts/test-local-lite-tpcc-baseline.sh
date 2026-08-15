#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
asset_dir="$repo_root/benchmarks/tpcc"
properties="$asset_dir/qualification.properties"
manifest="$asset_dir/schema-manifest.tsv"
deviations="$asset_dir/dialect-deviations.tsv"
report=${TPCC_BASELINE_REPORT:-}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

for required in "$asset_dir/README.md" "$properties" "$manifest" "$deviations"; do
  [[ -s "$required" ]] || fail "missing TPC-C baseline asset: $required"
done

property() {
  local key=$1
  local value
  value=$(awk -F= -v key="$key" '$1 == key { print substr($0, index($0, "=") + 1) }' "$properties")
  [[ -n "$value" ]] || fail "missing property: $key"
  printf '%s' "$value"
}

[[ "$(property contract.version)" == 1 ]] || fail "unsupported contract version"
[[ "$(property specification.version)" == 5.11.0 ]] || fail "specification is not pinned"
[[ "$(property claim.level)" == tpc-c-like ]] || fail "claim boundary is not explicit"
[[ "$(property warehouses)" == 1 ]] || fail "M14 starts at one warehouse"
[[ "$(property districts.per.warehouse)" == 10 ]] || fail "district cardinality drift"
[[ "$(property customers.per.district)" == 3000 ]] || fail "customer cardinality drift"
[[ "$(property orders.per.district)" == 3000 ]] || fail "order cardinality drift"
[[ "$(property new.orders.per.district)" == 900 ]] || fail "new-order cardinality drift"
[[ "$(property items)" == 100000 ]] || fail "item cardinality drift"

entity_count=$(awk -F '\t' 'NR > 1 && NF { count++ } END { print count + 0 }' "$manifest")
[[ "$entity_count" == 9 ]] || fail "expected nine logical entities, found $entity_count"
duplicate_entities=$(awk -F '\t' 'NR > 1 { seen[$1]++ } END { for (key in seen) if (seen[key] != 1) print key }' "$manifest")
[[ -z "$duplicate_entities" ]] || fail "duplicate logical entities: $duplicate_entities"

for entity in WAREHOUSE DISTRICT CUSTOMER HISTORY NEW_ORDER ORDER ORDER_LINE ITEM STOCK; do
  awk -F '\t' -v entity="$entity" 'NR > 1 && $1 == entity { found=1 } END { exit !found }' "$manifest" ||
    fail "manifest is missing $entity"
done

deviation_count=$(awk -F '\t' 'NR > 1 && NF { count++ } END { print count + 0 }' "$deviations")
((deviation_count >= 7)) || fail "known deviations are not fully recorded"

warehouses=$(property warehouses)
districts=$((warehouses * $(property districts.per.warehouse)))
customers=$((districts * $(property customers.per.district)))
orders=$((districts * $(property orders.per.district)))
new_orders=$((districts * $(property new.orders.per.district)))
items=$(property items)
stock=$((warehouses * items))

json=$(printf '{"contract_version":1,"specification":"TPC-C 5.11.0","claim":"tpc-c-like","seed":%s,"warehouses":%s,"expected_rows":{"WAREHOUSE":%s,"DISTRICT":%s,"CUSTOMER":%s,"HISTORY":%s,"NEW_ORDER":%s,"ORDER":%s,"ITEM":%s,"STOCK":%s},"order_line_rows":"deterministic-after-generation","deviations":%s}' \
  "$(property data.seed)" "$warehouses" "$warehouses" "$districts" "$customers" "$customers" "$new_orders" "$orders" "$items" "$stock" "$deviation_count")

if [[ -n "$report" ]]; then
  printf '%s\n' "$json" >"$report"
else
  printf '%s\n' "$json"
fi

echo "LocalLite M14A TPC-C baseline contract checks passed"
