#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/scripts/NativeLiteTpccTransactions.java"
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'SELECT OL_SUPPLY_W_ID,OL_I_ID FROM TPCC_ORDER_LINE' "$source_file" ||
  fail "Stock-Level range query is missing"
grep -q 'SELECT S_QUANTITY FROM TPCC_STOCK' "$source_file" ||
  fail "Stock-Level point lookup is missing"
grep -q 'STOCK_LEVEL_ORDER_WINDOW = 20' "$source_file" ||
  fail "Stock-Level order window is not pinned"
grep -q 'Set<Integer> qualifyingItems' "$source_file" ||
  fail "Stock-Level item-id distinct aggregation is missing"
if grep -q 'COUNT(DISTINCT L.OL_I_ID)' "$source_file"; then
  fail "Stock-Level driver still contains the full-scan join"
fi

echo "LocalLite M16C Stock-Level two-phase source checks passed"
