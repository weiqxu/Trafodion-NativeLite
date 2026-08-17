#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/scripts/NativeLiteTpccTransactions.java"
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'SELECT I.I_ID,I.I_PRICE,S.S_QUANTITY,S.S_YTD' "$source_file" ||
  fail "New-Order item/stock joined batch lookup is missing"
grep -q 'S.S_W_ID=? AND S.S_I_ID=I.I_ID' "$source_file" ||
  fail "New-Order item/stock join predicate is missing"
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

echo "LocalLite New-Order batch source checks passed"
