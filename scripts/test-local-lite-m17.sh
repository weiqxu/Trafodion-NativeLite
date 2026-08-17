#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fail() { echo "FAIL: $*" >&2; exit 1; }

transactions="$repo_root/scripts/NativeLiteTpccTransactions.java"
occ="$repo_root/core/sql/localstore/LocalLiteRocksDBStore.cpp"
design="$repo_root/plan/local-lite-tpcc-m17-design.md"

grep -q 'SELECT D.D_NEXT_O_ID,W.W_TAX,C.C_DISCOUNT' "$transactions" ||
  fail "New-Order header reads are not coalesced"
grep -q 'new-order header returned extra rows' "$transactions" ||
  fail "New-Order header cardinality check is missing"
grep -q 'indexedHistory_' "$occ" ||
  fail "OCC object history index is missing"
grep -q 'removeIndexedSequence' "$occ" ||
  fail "OCC history index cleanup is missing"
grep -q 'M17' "$design" ||
  fail "M17 design document is missing"

echo "LocalLite M17 source and design checks passed"
