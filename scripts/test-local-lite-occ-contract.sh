#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
contract="$repo_root/benchmarks/tpcc/occ-contract.tsv"
metrics="$repo_root/benchmarks/tpcc/occ-metrics.tsv"

test -s "$contract"
test -s "$metrics"

require_contract() {
  local key=$1
  local value=$2
  awk -F '\t' -v key="$key" -v value="$value" \
    '$1 == key && $2 == value { found = 1 } END { exit !found }' "$contract"
}

require_contract isolation_model trafodion_mvcc_occ
require_contract validation_scope committed_writes_after_start_sequence
require_contract conflict_policy read_range_intersects_committed_write_key
require_contract write_write_policy writes_are_added_to_read_set
require_contract retry_sqlstate 40001
require_contract sscc disabled
require_contract rocksdb_optimistic_transactiondb disabled

while IFS=$'\t' read -r metric unit required; do
  if [[ "$metric" == metric ]]; then
    continue
  fi
  [[ -n "$metric" && -n "$unit" && "$required" == yes ]]
done < "$metrics"

grep -q 'void checkConflict' \
  "$repo_root/core/sqf/src/seatrans/hbase-trx/src/main/java/org/apache/hadoop/hbase/regionserver/transactional/TrxTransactionState.java.tmpl"
grep -q 'state.getStartSequenceNumber()' \
  "$repo_root/core/sqf/src/seatrans/hbase-trx/src/main/java/org/apache/hadoop/hbase/coprocessor/transactional/TrxRegionEndpoint.java.tmpl"

printf '{"contract_version":1,"milestone":"M15A","isolation_model":"trafodion_mvcc_occ","sscc":"disabled","status":"pass"}\n'
