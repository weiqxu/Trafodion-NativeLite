#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/scripts/NativeLiteTpccTransactions.java"
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'for (int order = Math.max(1, next - STOCK_LEVEL_ORDER_WINDOW)' \
  "$source_file" || fail "Stock-Level bounded order window is missing"
grep -q 'RUNTIME_ORDER_ITEMS.get(' "$source_file" ||
  fail "Stock-Level runtime order-item mapping is missing"
grep -q 'loadedOrderLineCount(WAREHOUSE, district, order)' "$source_file" ||
  fail "Stock-Level loaded order-line cardinality mapping is missing"
grep -q 'loadedOrderItem(district, order, line)' "$source_file" ||
  fail "Stock-Level deterministic loaded item mapping is missing"
grep -q 'SELECT S_I_ID,S_QUANTITY FROM TPCC_STOCK' "$source_file" ||
  fail "Stock-Level batch point lookup is missing"
grep -q 'S_I_ID IN (' "$source_file" ||
  fail "Stock-Level batch point lookup is missing"
grep -q 'STOCK_LEVEL_ORDER_WINDOW = 20' "$source_file" ||
  fail "Stock-Level order window is not pinned"
grep -q 'STOCK_LEVEL_MAX_KEYS' "$source_file" ||
  fail "Stock-Level bounded key cap is missing"
grep -q 'Set<Integer> qualifyingItems' "$source_file" ||
  fail "Stock-Level item-id distinct aggregation is missing"
if grep -q 'COUNT(DISTINCT L.OL_I_ID)' "$source_file"; then
  fail "Stock-Level driver still contains the full-scan join"
fi

echo "Lite M16C Stock-Level two-phase source checks passed"
