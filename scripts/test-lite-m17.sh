#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fail() { echo "FAIL: $*" >&2; exit 1; }

transactions="$repo_root/scripts/NativeLiteTpccTransactions.java"
occ="$repo_root/core/sql/litestore/LiteRocksDBStore.cpp"
design="$repo_root/plan/lite-tpcc-m17-design.md"

grep -q 'SELECT C.C_DISCOUNT,D.D_NEXT_O_ID' "$transactions" ||
  fail "New-Order header reads are not coalesced"
grep -q 'W_TAX is immutable for this workload' "$transactions" ||
  fail "New-Order immutable Warehouse dependency is not documented"
grep -q 'new-order header does not exist' "$transactions" ||
  fail "New-Order header existence check is missing"
grep -q 'indexedHistory_' "$occ" ||
  fail "OCC object history index is missing"
grep -q 'removeIndexedSequence' "$occ" ||
  fail "OCC history index cleanup is missing"
grep -q 'M17' "$design" ||
  fail "M17 design document is missing"

echo "Lite M17 source and design checks passed"
