# Trafodion Lite M22: Concurrent Commit and Full-Scale Qualification

Status: complete. M22A-M22H pass without relaxing durability, cardinality,
latency, throughput, or recovery thresholds.

## Completion contract

M22 qualifies the complete 10-warehouse input cardinality (10 districts per
warehouse, 3,000 customers and orders per district, and 100,000 items) with 32
business terminals. Three measured repetitions must sustain at least 50 TPS
with at most 10% throughput variance. Profile p95 limits are 1 second
New-Order, 500 ms Payment, 500 ms Order-Status, 2 seconds Delivery, and 2
seconds Stock-Level. Retry exhaustion, unclassified errors, false OCC
conflicts, and consistency failures are forbidden.

The result remains TPC-C-like engineering evidence. It is not official tpmC or
formal TPC-C compliance.

## M22A: stage and conflict telemetry

Record commit-latch wait, OCC validation, referential-integrity validation,
write-batch construction, durable RocksDB write, and visibility-publication
latencies. Classify validation conflicts as point, bounded-range, or full-scan
conflicts. Reports must remain machine-readable and preserve the exact source,
configuration, host, and workload identity.

## M22B: precise OCC conflicts

Index committed writes by object and exact encoded key, normalize every read
range, and prove that disjoint keys do not conflict while point overlap,
phantoms, and write skew still abort deterministically. Legitimate workload
conflicts may be retried; false conflicts and retry exhaustion are forbidden.

## M22C: concurrent durable publication

Replace the process-wide physical-commit latch with deterministic key-stripe
commit intents. Acquire all stripes in canonical order, revalidate after
acquisition, commit one atomic TransactionDB batch, and publish visibility
before releasing the intents. Disjoint transactions must overlap physically;
overlapping point/range transactions must serialize or abort. Fault gates cover
the durable-decision and visibility boundaries. A serial emergency switch is
retained, but qualification uses the parallel path.

## M22D: resumable parallel native loader

Generate and publish warehouse-local partitions concurrently. A durable
manifest identifies table, warehouse/district, key range, row count, checksum,
and completion state. Restart skips only verified completed partitions. The
loader continues through the normal key, index, foreign-key, OCC, and recovery
contracts and must complete the full cardinality within the declared one-hour
controlled-host window.

## M22E: bound key plans and concurrent compilation

Carry parameter values into primary/UNIQUE/secondary-index access descriptors
without literal SQL specialization. Repeated prepared execution reuses one
plan. Move mutable compiler state to session or thread ownership and retain
only narrow DDL/catalog locks. The gate requires zero keyed prepared full scans
and at least four observed concurrent DML plan constructions.

## M22F: full qualification

Load full cardinality, warm up at least 20 transactions per terminal, measure
at least 100 transactions per terminal for three repetitions, and retain p50,
p95, p99, throughput, conflict, retry, resource, checkpoint, clean/unclean
restart, restore, disk-watermark, and consistency evidence. Synchronous commit
is mandatory.

## M22G: aggregate gates and evidence consistency

Expose M18-M22 aggregate Make targets, retain compact qualification evidence
with checksums, and keep README, transaction roadmap, milestone design, and
Makefile status synchronized by an automated contract check.

Completed by `scripts/test-lite-m22g.sh`. The compact evidence record is
`benchmarks/tpcc/m22-qualification-evidence.json`; it binds the exact
qualification configuration and retained full reports by SHA-256.

## M22H: release audit

Run Debug and Release builds, M10-M22 gates, native and allowlisted legacy
regressions, T4 JDBC, OCC/isolation, fault recovery, and the full qualification.
M22 remains active if any prerequisite, performance target, report-integrity
check, or source-tree cleanliness check fails.

`scripts/test-lite-m22h.sh` is the authoritative clean-tree release
audit. It records every gate in an external audit log and binds the final full
qualification reports to the audited Git revision by SHA-256.

## Required commit organization

Each phase is one focused commit named `feat(lite): complete M22X ...`
or `test/docs(lite): complete M22X ...`. Commit bodies explain the
delivered behavior, implementation method, and validation evidence. Threshold
relaxations and reduced workloads are diagnostic results, never completion.
