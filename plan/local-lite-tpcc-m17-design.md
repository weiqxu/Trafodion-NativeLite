# M17: New-Order execution path and OCC validation optimization

## Status

M17 is the optimization milestone after M16's Stock-Level range path. It keeps
the Trafodion MVCC/OCC model and the unified TransactionDB layout unchanged.
The implementation has two production-facing changes:

1. New-Order coalesces the district next-order-id, warehouse tax, and customer
   discount reads into one prepared six-parameter join. These values are read
   from one transaction snapshot and the result is required to contain exactly
   one row. The write phase remains atomic and still batches stock updates and
   order-line inserts.
2. OCC committed writes are indexed by table/object UID. Validation walks only
   the indexed writes for each transaction read range instead of scanning every
   committed transaction and then filtering unrelated objects. The original
   history remains the overflow/accounting source; index entries are removed
   together with collected history entries, including the history-overflow path.

This is an optimization only: no SSCC, pessimistic locking, or RocksDB
OptimisticTransactionDB is introduced. Read-range intersection, table-level
writes, post-start sequence checks, retryable `40001` diagnostics, and atomic
publication are unchanged.

## Baseline and target

The latest M16 Release qualification before M17 is a repeatable TPC-C-like
engineering run, not an official TPC-C or `tpmC` result:

- 32 warehouses and 32 terminals, five profiles, 10 districts, 100 customers,
  1,000 items, and 30 measured transactions per terminal;
- 19.516 TPS with 0.012002 three-run variance and zero OCC conflicts;
- New-Order p95 1,991.837 ms; Payment 1,158.004 ms; Order-Status 892.086 ms;
  Delivery 1,612.144 ms; Stock-Level 1,291.804 ms;
- 12,286 validation candidates, 1.442 s aggregate validation latency, and
  3.057 s aggregate publication latency.

M17 records, but does not silently relax, the engineering targets of at least
50 TPS and New-Order p95 at or below 1 second. A run may be called qualified
only when the correctness gates pass and the report records the observed
throughput, p95, retries, conflicts, scan counters, and environment. Formal
TPC-C compliance and `tpmC` claims remain excluded.

The M17 Release gate passed on 2026-08-17. It measured 19.667 TPS with
0.045290 three-run variance, New-Order p95 1,884.007 ms, Payment p95
1,384.527 ms, Order-Status p95 908.099 ms, Delivery p95 1,954.488 ms, and
Stock-Level p95 1,523.906 ms. There were zero OCC conflicts, retries, and
unclassified errors, and Stock-Level full scans remained zero. Validation
latency was 218.735 ms aggregate and publication latency was 3.529 s. The
validation-candidate counter now counts indexed write/range candidates rather
than whole committed transactions, so its 20,181,545 value is not directly
comparable to the M16 baseline's 12,286; the latency and correctness counters
are the comparable evidence.

## Correctness and performance gates

The staged gate is:

```text
make local-lite-m17a   # source/design contract
make local-lite-m17b   # T4 lifecycle and five-profile transaction checks
make local-lite-m17c   # Release TPCC-like performance and operations run
make local-lite-m17    # aggregate M17 gate
```

The performance command writes its JSON evidence to
`/tmp/traf-local-lite-m17-report` by default. The report must retain the
M14/M15/M16 claim boundary, show zero unclassified errors, preserve Stock-Level
zero-full-scan evidence, and include the OCC validation/publication counters.

## Rollback and follow-up

The New-Order query change can be reverted independently because it only changes
the read statement and cardinality check. The OCC index is guarded by the
existing validation contract and can be disabled by restoring the previous
history walk if a backend-specific ordering regression is found. M17 is
implementation-complete but does not meet the 50 TPS / 1-second targets. M18
should measure per-stage T4 prepare/execute latency and investigate
publication-latch contention after M17's reduced validation candidate set is
benchmarked.
