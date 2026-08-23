#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
schema="$repo_root/benchmarks/tpcc/schema.sql"
contract="$repo_root/benchmarks/tpcc/stock-level-contract.tsv"

fail() { echo "FAIL: $*" >&2; exit 1; }

[[ -s "$schema" ]] || fail "missing TPC-C schema"
[[ -s "$contract" ]] || fail "missing M16 contract"

awk '
  /CREATE INDEX TPCC_ORDER_LINE_STOCK_IX/ { found=1 }
  found && /ON TPCC_ORDER_LINE \(OL_W_ID, OL_D_ID, OL_O_ID, OL_SUPPLY_W_ID, OL_I_ID\);/ { matched=1 }
  END { exit matched ? 0 : 1 }
' "$schema" || fail "index declaration did not match exactly"

grep -q $'^order_line_access\tTPCC_ORDER_LINE_STOCK_IX\t' "$contract" ||
  fail "contract does not name the Stock-Level index"
grep -q $'^order_window\t20\t' "$contract" ||
  fail "contract order window drifted"

index_count=$(grep -c '^CREATE INDEX TPCC_ORDER_LINE_STOCK_IX$' "$schema" || true)
[[ "$index_count" == 1 ]] || fail "expected one Stock-Level index declaration"
echo "Lite M16B Stock-Level index contract checks passed"
