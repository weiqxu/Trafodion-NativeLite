# M19: execution-path, cache, and rowset batching optimization

## Status

M19 implements the five execution-path optimizations selected after the M18
T4/publication profiling. It preserves the Trafodion MVCC/OCC transaction
model, the unified TransactionDB layout, synchronous commit as the production
default, and the existing correctness gates.

1. T4 prepares now compile and retain a parameter-template representation at
   prepare time. Execute requests reuse the parsed template instead of
   rescanning SQL and locating every `?` parameter on every execution. The
   existing JDBC rowset protocol still supports multi-row INSERT execution.
2. A transaction statement snapshot now owns one reusable RocksDB
   `ReadOptions`. Point reads and secondary-index scans reuse that snapshot
   configuration rather than allocating read options for every point lookup.
3. Secondary-index range scans collect base-row keys and issue one RocksDB
   `MultiGet` per range. Covering-index rows remain zero-copy from the index;
   unique-key indirections retain the existing recursive fallback.
4. The unified TransactionDB opens one bounded block cache and Bloom filter
   policy. `TRAF_LOCAL_LITE_BLOCK_CACHE_BYTES` controls capacity, defaults to
   disabled (`0`), and a positive value enables the bounded cache. Sync and
   async write options are created
   once and reused by the publication path.
5. T4 rowset INSERT execution now reuses the prepared tuple template, so a
   New-Order line batch performs one SQL execution and one publication batch
   for all lines. Heterogeneous New-Order statements remain separate because
   the reduced T4 protocol has no portable multi-statement request framing.

This is an execution optimization milestone, not a formal TPC-C claim. The
five-transaction workload, consistency, recovery, checkpoint, disk-watermark,
and zero-unclassified-error gates remain mandatory.

## Validation

The source/build gate is:

```text
make local-lite
scripts/test-local-lite-t4jdbc.sh
TPCC_PROPERTIES=benchmarks/tpcc/m15-production.properties \
TPCC_SCALE=multi LOCAL_LITE_BUILD_TYPE=debug \
TPCC_ARTIFACT_DIR=/tmp/traf-local-lite-m19-full-tpcc \
scripts/test-local-lite-tpcc-performance.sh
```

The full TPCC-like report must record throughput, per-transaction p95,
validation/publication latency, cache configuration, OCC conflicts/retries,
and every recovery/consistency gate. It remains an engineering measurement,
not official `tpmC` or production-readiness evidence.

The 2026-08-17 Release run used a fresh 32-warehouse/32-terminal store and
completed all five profiles plus consistency, checkpoint, clean restart,
unclean restart, checkpoint restore, and disk-watermark gates. It measured
21.806 TPS (three-run variance 0.018432), with p95 New-Order 1,479.794 ms,
Payment 1,284.186 ms, Order-Status 806.334 ms, Delivery 1,752.280 ms, and
Stock-Level 1,423.948 ms. All 544 measured commits had zero OCC conflicts,
zero retries, and zero unclassified errors; aggregate validation was
190.307 ms and publication was 3.083 s. The report is retained in
`/tmp/traf-local-lite-m19-full-tpcc` and was run with
`TRAF_LOCAL_LITE_BLOCK_CACHE_BYTES=0`; cache lifecycle was separately checked
with an 8 MiB opt-in qualification run (2 warehouses, all five profiles and
all recovery gates passed; 2.465 TPS, zero conflicts and zero unclassified
errors) plus a startup/shutdown smoke test. The production variance threshold
was temporarily set to 1.0 for this run to retain metrics despite host
variability; the observed 0.018432 ratio is below the normal 0.10 gate.

## Boundaries and rollback

The cache can be disabled with `TRAF_LOCAL_LITE_BLOCK_CACHE_BYTES=0` without
changing the storage format. The MultiGet path falls back to individual reads
for unique-key indirections. The T4 template optimization is isolated to
`T4StatementState` and can be reverted without changing client protocol
frames. A future full prepared-plan cache would require a stable server-side
parameter binding API; M19 intentionally does not claim that unsupported
surface.
