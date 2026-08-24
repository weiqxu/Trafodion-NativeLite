#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_file="$repo_root/scripts/TrafodionLiteTpccTransactions.java"
workload_file="$repo_root/scripts/TrafodionLiteTpccWorkload.java"
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'stockLevelRangeScans' "$source_file" ||
  fail "Stock-Level range telemetry is missing"
grep -q 'stockLevelPointReads' "$source_file" ||
  fail "Stock-Level point-read telemetry is missing"
grep -q 'stockLevelBatchReads' "$source_file" ||
  fail "Stock-Level batch-read telemetry is missing"
grep -q 'stock_level_access' "$workload_file" ||
  fail "workload report does not expose Stock-Level access telemetry"
grep -q 'full_scans.*0' "$workload_file" ||
  fail "Stock-Level zero-full-scan declaration is missing"

echo "Lite M16E telemetry and qualification contract checks passed"
