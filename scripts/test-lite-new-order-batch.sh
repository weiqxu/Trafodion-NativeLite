#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/scripts/NativeLiteTpccTransactions.java"
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'SELECT I_ID,I_PRICE FROM TPCC_ITEM WHERE I_ID IN (' "$source_file" ||
  fail "New-Order item static multi-get is missing"
grep -q 'SELECT S_I_ID,S_DIST_01 ' "$source_file" ||
  fail "New-Order stock projection is missing"
grep -q 'FROM TPCC_STOCK WHERE S_W_ID=' "$source_file" ||
  fail "New-Order stock static multi-get is missing"
grep -q 'for (int item : uniqueItems)' "$source_file" ||
  fail "New-Order unique item batching is missing"
grep -q 'S_QUANTITY=CASE' "$source_file" ||
  fail "New-Order stock batch update is missing"
grep -q 'INSERT INTO TPCC_ORDER_LINE VALUES ' "$source_file" ||
  fail "New-Order line batch insert is missing"
if grep -q 'SELECT I_PRICE FROM TPCC_ITEM WHERE I_ID=?' "$source_file"; then
  fail "New-Order still performs per-line item lookups"
fi
if grep -q 'SELECT S_QUANTITY,S_YTD,S_ORDER_CNT,S_DIST_01 FROM TPCC_STOCK ' \
    "$source_file"; then
  fail "New-Order still performs per-line stock lookups"
fi

echo "Lite New-Order batch source checks passed"
