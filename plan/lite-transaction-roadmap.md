# Trafodion Lite Transaction and Concurrency Roadmap

## Purpose

This document defines how Trafodion Lite should evolve from the current
single-process RocksDB smoke path into a storage layer that matches Trafodion's
executor transaction boundaries.

The immediate goal is not full TMF/DTM/RMS integration. Lite Storage currently
does not start the Trafodion transaction service stack. The practical goal is to
introduce the same shape of ownership that Trafodion uses elsewhere:

- The executor owns statement and transaction boundaries.
- Storage receives an explicit operation context.
- Reads and writes go through executor TCBs.
- RocksDB is hidden behind a Lite Storage API.
- Lite Storage can later replace the storage backend without changing executor
  TCB semantics.

## Current Baseline

As of M14E (verified 2026-08-16), Trafodion Lite supports the following Lite Storage table
and transactional-storage path:

- `CREATE TABLE` and `DROP TABLE` are routed through compiler DDL code and write
  Lite Storage catalog metadata.
- `INSERT INTO ... VALUES` and `INSERT ... SELECT` execute through
  `LiteHbaseInsertTcb`.
- `SELECT` executes through `LiteRocksdbScanTcb`.
- Executor scan can consume encoded `listOfGetRows()` row-key requests through
  `LiteTxn::getRowByKey()` and falls back to full scan when no get-row
  request is present.
- Persistent rows use the `LTBR1` binary aligned row payload.
- SQLCI no longer directly executes supported Lite Storage table queries against
  RocksDB.

The important storage limits are:

- `LiteStorageManager` owns process-local shared RocksDB catalog and table
  handles.
- Keyless table rows still use catalog `nextRowId` and an internal 8-byte row
  key.
- Primary-key Lite Storage tables store row data under deterministic binary row keys
  derived from the persisted `LTBR1` row payload.
- Each `ContextCli` owns its explicit Lite Storage transaction context and buffers
  pending writes until `COMMIT WORK`.
- RocksDB TransactionDB implements the selected backend-neutral storage
  contract, the exclusive catalog/table key space after M13 activation, and the
  durable commit journal used by the SQL publication protocol.
- All touched tables are conflict-checked before the durable commit decision;
  idempotent table markers let startup complete an interrupted multi-table DML
  publication before accepting traffic.
- Writing transactions record the unified TransactionDB sequence at snapshot
  creation and validate it at commit after exact row-before-image checks. This
  is a conservative serializable mechanism: it prevents predicate phantoms and
  write skew, but can reject independent concurrent writers.

This is a durable single-node boundary, not a distributed transaction system.
Cross-process attempts to open the same `TRAF_LITE_STORE_DIR` are rejected by
the RocksDB lock and surfaced as an explicit Trafodion Lite process-boundary
diagnostic. Fresh stores are created directly in the format-version-2
`transactiondb/` layout. Stores containing the obsolete per-table `catalog/`
or `data/` directories are rejected explicitly. The old-layout reader,
migration, export, rollback, and runtime fallback code is not present.

## Trafodion Transaction Model To Preserve

The existing Trafodion model has three useful boundaries for Trafodion Lite:

- `ExTransaction` represents the current executor transaction state and handles
  `BEGIN`, `COMMIT`, and `ROLLBACK`.
- HBase executor TCBs do not invent their own transaction semantics. They read
  the current transaction id from CLI context and pass it down to storage.
- Storage-specific code handles conflict detection, row locking, atomicity, and
  durability for that transaction context.

For Trafodion Lite, this means executor TCBs should not keep calling raw RocksDB
functions directly. They should call a Lite Storage transaction facade that can run in
autocommit mode today and explicit transaction mode later.

## Target Architecture

The target Lite Storage stack should have these layers:

```text
SQL statement
  -> compiler-generated plan
  -> executor TCB
  -> LiteTxn / LiteTxnManager
  -> LiteStorageManager
  -> shared RocksDB catalog/table handles
```

`LiteStorageManager` should provide process-local ownership of RocksDB
database handles:

- one shared catalog handle per Lite Storage root;
- one shared table handle per table path;
- reference-counted or process-lifetime handle ownership;
- per-table mutexes for metadata and row-id critical sections;
- a clear close/shutdown path for tests.

`LiteTxnManager` should provide the executor-facing transaction shape:

- create an autocommit transaction for statements without explicit SQL
  transaction state;
- bind to the current CLI transaction id when local explicit transactions are
  enabled;
- expose statement commit and rollback hooks;
- hide whether writes are immediate, batched, or buffered.

## Roadmap

### Phase 1: Shared RocksDB Handle Ownership

Status: completed for the Lite Storage v1 process boundary. Process-local handle
sharing is covered by the RocksDB SQLCI smoke self-join regression.

Fix the current same-process lock problem first.

Tasks:

- Add `LiteStorageManager`.
- Move catalog open/close ownership out of each `LiteRocksDBStore`
  instance.
- Cache table handles by table path.
- Make `putRow()` and `scanRows()` use shared table handles.
- Add process-local mutexes for catalog metadata updates and table writes.
- Keep the public behavior unchanged for existing DDL, INSERT, and SELECT.

Validation:

- Existing Trafodion Lite runtime smoke passes.
- Existing RocksDB SQLCI smoke passes.
- A self-join over one Lite Storage table no longer fails on RocksDB `LOCK`.
- Two scans over the same table in one query use executor scan TCBs and do not
  reopen the same RocksDB path incompatibly.

### Phase 2: Statement-Level Atomic Writes

Status: completed for Lite Storage v1 table INSERT, including multi-row
tuple-flow plans. Row-id allocation and row persistence go through
`LiteTxn::insertRow()`; an autocommit tuple flow targeting the Lite insert
TCB opens an implicit Lite Storage transaction, commits its pending rows only after
complete source/target EOD, and rolls back on source errors, target errors, or
cancellation. The old public `allocateRowId()` and direct `putRow()` store APIs
have been removed, so Lite Storage executor writes cannot bypass the transaction
facade. At Phase 2 completion, catalog metadata and table rows still lived in
separate RocksDB databases, so that phase did not claim cross-process or
crash-atomic multi-DB commit semantics. M12 later added the durable
compatibility journal for multi-table DML; it did not add cross-process
writers.
Same-process concurrent write coverage now exercises multiple writer threads
plus an overlapping scanner through the Lite Storage transaction facade and validates
contiguous, duplicate-free row ids.

Make every autocommit statement atomic at the Lite Storage layer.

Tasks:

- Introduce `LiteTxn` in autocommit mode.
- Replace direct `allocateRowId()` plus `putRow()` calls with one transaction
  operation such as `insertRow()`.
- Update catalog `nextRowId` and table row data in one RocksDB write batch when
  they belong to the same RocksDB database.
- If catalog and table data remain in separate RocksDB databases, protect the
  operation with a process-local critical section and document the remaining
  crash-consistency gap.
- Return statement-level errors before reporting row counts to the executor.

Validation:

- Multi-row `INSERT` either persists all accepted rows for the statement or
  returns an error before reporting success.
- A late primary-key conflict in autocommit `INSERT ... SELECT` leaves no rows
  from that statement and does not poison the following statement.
- Row id allocation does not duplicate under same-process concurrent inserts.
- Failed insert expression or constraint evaluation does not advance persisted
  row state for that row.

### Phase 3: Snapshot-Based Statement Reads

Status: completed for statement-wide reads of each Lite Storage table.
Executor root begin/end hooks identify a prepared statement execution with the
statement globals pointer plus execution count. `LiteTxn` uses that token
for both full scans and get-row access, and every scan TCB that reads the same
table in that execution reuses one RocksDB snapshot. The root releases all
snapshots at end-of-data, cancellation, fatal error, or TCB teardown. A later
execution receives a new context and new snapshots.

RocksDB snapshots are database-local. Because each Lite Storage table currently has a
separate RocksDB database, this phase gives repeatable reads for repeated access
to one table, but does not claim a single atomic snapshot instant across
different Lite Storage tables. Keyless scan metadata continues to expose the internal
RocksDB row id as a hidden `SYSKEY`, and the scan TCB materializes that system
value separately from the `LTBR1` user column payload.

Give scans a stable statement snapshot.

Tasks:

- Add snapshot acquisition to `LiteTxn` for read statements.
- Make `LiteRocksdbScanTcb` scan with the statement snapshot.
- Release snapshots when the statement finishes, errors, or is cancelled.
- Preserve current binary row materialization and projection mapping.

Validation:

- A scan sees a stable view while another same-process writer inserts rows.
- Repeated scans inside one statement observe the same snapshot when required
  by executor semantics.
- Predicate and projection tests continue to pass for all supported scalar
  types.
- The standalone statement snapshot probe verifies that full scan and get-row
  access do not see a row inserted after snapshot acquisition, while the next
  execution does.
- SQLCI self-join trace verifies one snapshot acquire, snapshot reuse by the
  second scan TCB, and release at statement completion.

### Phase 4: Explicit Local Transactions

Status: completed for the single-process Lite Storage v1 transaction boundary.
The implementation provides a repeatable-read context for Trafodion Lite SQLCI.
`BEGIN WORK` creates a pending write set and a transaction read context. The
first access to each table lazily acquires its RocksDB snapshot, and later
statements in the transaction reuse that snapshot for both full scans and
get-row reads. Pending INSERT rows are overlaid on that stable base view, so the
transaction continues to read its own writes. `ROLLBACK WORK` discards pending
rows and releases the read context. `COMMIT WORK` now computes and preflights
all pending primary and UNIQUE keys against committed storage before publishing
any row from that table, then writes its base rows and secondary uniqueness
records with one RocksDB `WriteBatch`.

For keyless tables, COMMIT verifies that provisional row ids still begin at the
current catalog `nextRowId`, updates the metadata while holding the storage
manager mutex, and rolls that update back if the table batch reports an error.
Catalog metadata and table rows remain separate RocksDB databases, so this
provides process-consistent failure handling but not crash-atomic publication
across those two databases.

Lite Storage also disables the HBase coprocessor `COUNT(*)` binder rewrite.
Transaction aggregates therefore remain normal executor aggregates over the
same local scan and transaction snapshot path.

At Phase 4 completion this phase did not provide RocksDB TransactionDB
semantics, atomic multi-table commit, crash-atomic keyless catalog/table
publication, or one cross-table snapshot instant because the catalog and each
table used separate RocksDB databases. M12 supersedes the multi-table DML
limitation with preflight plus a durable, idempotently replayed commit journal;
M13 later makes the single-keyspace target the exclusive runtime format.

Add Lite Storage transaction behavior for `BEGIN`, `COMMIT`, and `ROLLBACK` without
starting TMF/DTM/RMS.

Tasks:

- Teach Lite Storage transaction statements to create and finish a
  `LiteTxnContext` in CLI context.
- Route Lite Storage table writes into the current Lite Storage transaction context when one
  exists.
- Buffer writes in a local write set or use RocksDB TransactionDB.
- Make `COMMIT` atomically publish each table's transaction write set.
- Make `ROLLBACK` discard uncommitted writes.
- Make scans read from the transaction snapshot plus own writes.

Validation:

- `BEGIN; INSERT ...; ROLLBACK; SELECT ...` does not show rolled-back rows.
- `BEGIN; INSERT ...; COMMIT; SELECT ...` shows committed rows.
- A transaction can read its own writes.
- Separate SELECT statements in one transaction reuse the same per-table
  snapshot and do not observe a committed row added after the first read.
- Full scan and get-row access use the same transaction snapshot.
- Autocommit behavior remains unchanged outside explicit transactions.
- A committed primary or UNIQUE duplicate detected during COMMIT does not
  partially publish an earlier pending row from the same table.
- A failed keyless UNIQUE commit does not advance persisted `nextRowId`.

### Phase 5: Deterministic Row Keys And Conflict Detection

Status: completed for the Lite Storage v1 key and conflict-detection scope.
Lite Storage DDL now
accepts PRIMARY KEY and UNIQUE, persists key column ordinals in `LTT3` table
metadata, and rejects RI/CHECK constraints. INSERTs into primary-key tables
build a deterministic `P`-prefixed RocksDB row key from the binary aligned
`LTBR1` payload. UNIQUE constraints use `U`-prefixed secondary uniqueness
records in the same RocksDB table. Duplicate primary and unique keys are
rejected in committed data and inside the current pending Lite Storage transaction
write set. Keyless tables continue to use the existing internal row id path.
The Lite Storage executor scan TCB can now consume encoded get-row requests from
`listOfGetRows()` through `LiteTxn::getRowByKey()`, including read-own-write
lookups inside the pending Lite Storage transaction write set. Pre-code can now rewrite
constant primary-key equality search keys to deterministic `P`-prefixed
Lite Storage get-row keys. Binary NUMERIC, DECIMAL, and BigNum NUMERIC key
literals can now be encoded into deterministic get-row keys, including negative
predicate forms that the compiler represents as constant expressions rather than
plain `ConstValue`s. UNIQUE-key equality predicates can now be mapped to
deterministic `U`-prefixed get-row keys when the predicate supplies all columns
of one unique key. Lite Storage NATable synthesis now exposes primary-key and
UNIQUE-key metadata to the optimizer. UNIQUE keys are represented as logical
unique access paths that keep the physical scan name on the base Lite Storage table;
the executor resolves `U` records back to the persisted base `LTBR1` row.
Lite Storage DML also skips generic secondary-index maintenance because the
storage layer maintains `U` uniqueness records in the same RocksDB table.
UNIQUE get-row rewrite removes only predicates covered by the logical unique key
and leaves residual predicates for executor evaluation. Unsupported literal
types and non-key predicates still fall back to full executor scan. SQLCI smoke
now enables a test-only executor trace to assert that integer primary-key,
NUMERIC/DECIMAL/BigNum primary-key, and UNIQUE-key equality use the get-row path
while non-key predicates use full scan fallback. The same smoke also checks
EXPLAIN output for primary-key and UNIQUE-key equality subset scans, key column
metadata, single-probe access, and residual executor predicates.

Move closer to the original Trafodion HBase-style key model.

Tasks:

- Add primary key metadata support for Lite Storage tables.
- Generate local row keys from compiler/executor key expressions when a table
  has a primary key.
- Keep internal row id allocation only for keyless heap-like Lite Storage tables.
- Add duplicate-key detection for primary keys.
- Add conflict diagnostics that map cleanly to SQL errors.
- Add unique-key metadata and duplicate detection after primary-key storage is
  stable.
- Teach Lite Storage scan/get TCBs to consume optimized key access before exposing
  Lite Storage primary keys as optimizer-visible key metadata.
- Expose Lite Storage UNIQUE keys as optimizer-visible logical access paths while
  keeping executor scan storage on the base Lite Storage table.

Validation:

- Concurrent inserts of the same primary key cannot silently overwrite each
  other.
- Primary-key scans use deterministic encoded keys.
- Unique-key equality scans use deterministic encoded secondary keys.
- Unique-key equality scans preserve residual executor predicates.
- EXPLAIN output shows primary-key and UNIQUE-key equality as subset scans with
  key columns and single-probe access.
- Keyless table inserts continue to work through the internal row id path.
- `CREATE TABLE pk_t(a INT PRIMARY KEY, ...)`, `CREATE TABLE uq_t(...,
  UNIQUE(a))`, duplicate insert diagnostics, transaction read-own-write,
  rollback, primary-key/unique-key get-row trace assertions, explain subset-scan
  assertions, and keyless table regression are covered by the RocksDB SQLCI
  smoke.
- The same-process store concurrency probe starts simultaneous writers for one
  primary key and one UNIQUE key, verifies exactly one writer succeeds, and
  confirms failed UNIQUE attempts do not advance keyless `nextRowId`.

Phases 1 through 5 remain complete for the original v1 boundary. M12 builds on
them with a backend-neutral single-keyspace contract, TransactionDB selection,
and crash-recoverable multi-table DML publication. M13 adds restart-based,
format-checked activation of the exclusive catalog/table key space. Current
completion still does not claim cross-process concurrent writers, TMF
coordination, distributed execution, node HA, or zero-downtime upgrade
orchestration.

### Phase 6: Optional TMF Integration Boundary

Status: initial executor-bound transaction facade implemented. `LiteTxnManager`
now exposes executor-bound begin/commit/rollback entry points and records both a
Lite Storage transaction id and the executor transaction id token for the active local
transaction. The Lite Storage transaction TCB path calls these facade methods.
When a real `ExTransaction` id is available the facade can bind to it; in the
current TMF-disabled `lite` path the executor transaction object identity is
used as a local binding token. Scan and insert TCBs still talk only to
`LiteTxn` and do not receive TMF-specific state directly.

Only consider this phase after Trafodion Lite has stable Lite Storage transaction behavior.

Tasks:

- Keep `LiteTxnManager` as the only executor-to-storage transaction API.
- Add an implementation that can bind to Trafodion `ExTransaction` ids when a
  full TMF/DTM/RMS stack is available.
- Do not expose TMF-specific concepts directly inside Lite Storage table scan/insert
  TCBs.
- Add a real TMF-backed implementation after a full service stack is available.

Validation:

- Existing local autocommit and explicit Lite Storage transaction tests still pass.
- TMF-enabled builds can route transaction ids through the same facade.
- TMF-disabled `lite` builds do not require transaction service processes.
- Runtime structure checks assert that Lite Storage transaction statements call
  executor-bound facade methods.

## Design Decisions

### Do Not Start With Full TMF

Starting with full TMF would make Trafodion Lite depend on the service stack it is
designed to avoid. The first useful milestone is a Lite Storage transaction facade
with statement atomicity and stable reads.

### Do Not Keep Raw RocksDB Opens In TCB Paths

Executor TCBs should not own RocksDB open/close behavior. Shared handle
ownership is required for self-joins, concurrent scans, and predictable writer
behavior.

### Do Not Treat `nextRowId` As A Long-Term Concurrency Mechanism

The current `nextRowId` counter is acceptable only as a temporary keyless-table
identifier. Long-term row identity should come from deterministic key encoding
when table keys exist.

### Keep Binary Row Layout Independent From Transaction Metadata

The persisted `LTBR1` row payload should remain the table row value. Transaction
metadata, locks, versions, or write intents should live in storage keys,
side-records, RocksDB metadata, or the transaction manager layer.

## Implemented Foundation And Immediate M14 Step

The bounded transaction/storage phases, M1-M10 convergence gate, and M11
session/server boundary are complete. M11 uses the existing Trafodion ownership
chain rather than introducing a second session coordinator:

```text
ContextCli -> ExTransaction -> LiteTxnContext
             canonical state   RocksDB participant state
```

`ExTransaction` owns and synchronizes the Lite Storage transaction participant with
its existing executor transaction flags and ID. Scan/DML, DDL, tuple-flow, and
executor-root snapshot paths receive that participant explicitly. The mutable
`LiteTxnState` singleton is gone; only `LiteStorageManager` and store
handles remain process-owned. Reset, disconnect, delete, and destruction clean
up one session without affecting its peers. The effective authorization
identity and Trafodion Lite object ownership are read from that same current
`ContextCli`; the compiler session remains a propagation target, not a second
authority.

`make lite-m11` now combines the independent-context/SQLCI checks with a
long-running server gate and a real Trafodion T4 JDBC gate. The evidence includes two
overlapping client transactions, deterministic conflicts, disconnect rollback,
active cancel plus peer survival, prepared-statement reuse, typed fetch,
bounded catalog metadata, store-process
exclusivity, protected Unix-socket lifecycle, unnamed diagnostic capture, and
committed reads after clean and unclean restart.

M12 now provides backend-neutral `StorageEngine`, `StorageSession`,
`StorageTxn`, and streaming `StorageCursor` contracts. The shared gate covers
atomic catalog/base/UNIQUE/index records, snapshots, conflicts, cancellation,
durability, recovery, backup/restore, integrity, metrics, and disk watermarks
for RocksDB TransactionDB and SQLite WAL; TransactionDB is selected on
correctness, recovery, and operational-continuity grounds. Version-2 metadata
keys remove fixed-buffer truncation and are migrated with collision and exact
count checks.

The live SQL publication path is protected by a synchronous
TransactionDB commit journal. A transaction validates every touched table
before the durable commit decision, publishes table batches under one process
latch, and records an idempotent marker with each batch; startup replays only
committed incomplete journals. M13 now durably activates the format-version-2
single-keyspace target and rejects old per-table directories; no old-layout
conversion or fallback implementation remains. Online upgrade/drain
orchestration, active-layout backup scheduling, and journal consolidation
remain separate productization work. This remains a single-node boundary, not
node-level HA or distributed execution.

M14 TPC-C-like qualification is complete for the declared single-node boundary.
M14D proves the Level 3 boundary for the current functional profiles using
stable snapshots, exact key conflicts, and the disclosed database-wide
sequence validator. Its matrix covers dirty writes, dirty reads,
non-repeatable reads, predicate phantoms, write skew, bounded retry, and
durable-decision crash recovery. M14E must now remove the compiler/executor
serialization bottleneck without weakening that storage boundary. M14E now
does so: request threads own their selected ContextCli and thread-local CLI,
SQLCI, assertion, and schema state; only DDL/catalog mutation and compatibility
utilities retain narrow locks. The executable high-water mark proves at least
two concurrent compiler/executor requests, with M14C/M14D/T4 lifecycle
regressions preserving the transaction and cancellation boundaries.

M14F now adds two-warehouse operation with two terminal sessions, a fixed
45/40/5/5/5 functional mix, latency/throughput/resource reports, online
checkpoint, clean/unclean restart, restored-checkpoint verification, and
disk-watermark rejection. The workload uses fair writer admission around whole
transactions to avoid the disclosed database-wide validator's false
independent conflicts. These are explicit non-compliant performance boundaries,
not `tpmC`. M14G now makes M10-M13 and M14A-M14F explicit aggregate inputs,
embeds the workload/operations evidence in one report, and separates functional,
TPC-C-like, and failed formal-compliance claims.

M15 is complete for its declared correctness and repeatability boundary. It
replaces the database-wide validator and client writer admission with
Trafodion's MVCC/OCC rule: one transaction snapshot, point and predicate read
sets, and validation only against writes committed after the start sequence.
TransactionDB remains the atomic storage layer; M15 introduces neither SSCC nor
RocksDB OptimisticTransactionDB.

The seven independently committed phases cover contract/metrics,
transaction-wide snapshots, read/write sets, OCC validation, transactional
index reads, atomic delta publication, and Release TPC-C-like qualification.
`make lite-m15g` passes at 32 warehouses and 32 offset terminal schedules
with all five profiles represented, no client admission, no conflicts/retries,
and no unclassified errors. The 2026-08-16 baseline is 1.130 TPS with 3.423%
variance; Stock-Level p95 is 135.075 s. Therefore M15 is functionally complete
but does not meet the separate production targets of 50 TPS and
1/0.5/0.5/2/2-second p95. Official TPC-C and `tpmC` claims remain excluded.

The next transaction/productization order is:

1. Complete M17's New-Order execution-path and OCC validation optimization.
2. Reach the recorded 50 TPS and per-profile p95 production targets on a
   controlled host with longer samples and full environment disclosure.
3. Add service drain/upgrade, active-layout backup controls, recovery-journal
   consolidation, password/TLS, and operational SLO monitoring.
4. Keep `make lite-m15g` and the M10-M14 prerequisites as regression
   gates while widening the portable SQL surface.

The aggregate transaction gate is `make lite-m15`; detailed A-G phase
and claim boundaries are authoritative in
`plan/lite-legacy-regress-roadmap.md`.

## Milestone 16: Stock-Level range aggregation and index optimization

M16 is the first optimization milestone after the M15 OCC baseline. It targets
the observed Stock-Level p95 of 135.075 seconds and the associated full-scan
path; it does not change the Trafodion MVCC/OCC contract or introduce SSCC.

The implementation is deliberately two-phase: an ordered range over the last
20 orders in `TPCC_ORDER_LINE`, followed by primary-key point reads in
`TPCC_STOCK` for distinct `(supply warehouse,item)` pairs. Both phases remain
inside the same transaction-wide snapshot, and the result is required to be
equivalent to the original join/count-distinct statement.

M16A-M16G are independently committed: contract and plan, schema/index,
transaction query path, optimizer-plan proof, qualification gate, performance
evidence, and final documentation. The implementation, SQLCI plan gate, and
Release runtime gate are complete. The latest run measured 19.516 TPS with
0.012002 variance and Stock-Level p95 1.292 seconds with zero Stock-Level full
scans. The separate 50 TPS and 2-second production targets remain targets and
are not claimed as met.

## Milestone 17: New-Order execution path and OCC validation optimization

M17 follows the measured M16 result: Stock-Level is no longer the dominant
profile, while New-Order still pays for three independent header reads and OCC
validation scans unrelated committed objects. The authoritative design is
`plan/lite-tpcc-m17-design.md`.

The first M17 change coalesces district next-order-id, warehouse tax, and
customer discount into one prepared six-parameter join. It preserves one
transaction snapshot and exact-one-row semantics, while reducing T4 request
round trips before the write phase. The second change coalesces the ITEM price
and STOCK state batch reads into one prepared join. The third change indexes
committed OCC writes by object UID; validation probes only the object IDs
present in the transaction read set, and index cleanup follows history
collection and overflow eviction exactly.

M17A-M17C are the source/design contract, T4 and five-profile correctness, and
Release performance gates. `make lite-m17` is the aggregate command. The
latest run measured 21.332 TPS and 1.479 seconds New-Order p95, with zero
conflicts, retries, unclassified errors, and Stock-Level full scans. The 50 TPS
and New-Order p95 <=1 second values remain engineering targets, not
qualification claims; official TPC-C and `tpmC` remain explicitly excluded.

## Milestone 18: T4 execution and durable publication critical section

M18 is active after the M17 New-Order/OCC work. It keeps Trafodion's MVCC/OCC
validation and the unified TransactionDB atomic write batch unchanged while
targeting two measured costs: repeated T4 transaction-control compilation and
the synchronous WAL/publication critical section.

The T4 server now fast-paths `BEGIN`, `COMMIT`, and `ROLLBACK` through the
existing Lite `ExTransaction` participant only after the session context
has been initialized. DDL boundaries and uninitialized contexts use the
original executor path, preserving catalog invalidation and first-use session
initialization. Commit/rollback cursor-close behavior is retained.

`TRAF_LITE_SYNC_COMMIT` is an explicit A/B switch. The default is
`true`; only an opt-in false value disables RocksDB write-option sync and is
allowed for diagnostic upper-bound measurements. The publication latch is
released before transaction-store cleanup, but remains held across validation,
the physical batch write, and OCC publication, so atomicity is not weakened.

The first M18 evidence on the controlled WSL2 host is 19.464 TPS with sync
enabled versus 20.173 TPS in the async diagnostic run. Aggregate publication
latency falls from 3.928 s to 0.144 s, but throughput improves only 3.6%;
therefore WAL sync is a large commit-latency component but not the sole current
throughput bottleneck. Both runs pass consistency and restart/checkpoint gates,
and the T4 JDBC lifecycle gate passes. M18 remains below the 50 TPS and
per-profile production targets; RocksDB cache/write-stall and per-stage T4
timing instrumentation are the next actions.

## Milestone 19: Execution-path and rowset optimization

M19 adds prepared parameter-template reuse, snapshot ReadOptions reuse,
secondary-index MultiGet, an opt-in bounded block cache/Bloom policy, and
prepared rowset INSERT batching. Its aggregate gate is `make lite-m19`;
the design and rollback boundaries remain in
`plan/lite-tpcc-m19-design.md`.

## Milestone 20: Retained plans and server batches

M20 owns T4 prepared CLI plans and input descriptors in the session, executes
safe statements without re-prepare, and supports quoted-string-aware server
batches. Keyed predicates retained a documented specialization boundary until
M22E supplied executor-bound primary keys. Run `make lite-m20`.

## Milestone 21: Thread-affine multi-worker execution

M21 gives every accepted T4 connection a thread-affine CLI/compiler/
transaction context and bounds capacity with reusable session slots. The
native loader established complete-cardinality generation, while serialized
publication and compiler construction became the explicit M22 bottlenecks.
Run `make lite-m21` or `make lite-m21-tpcc`.

## Milestone 22: Concurrent commit and full-scale qualification

M22A-M22G are complete and M22H is the active release audit. M22 records
commit-stage telemetry, validates exact OCC conflicts, publishes disjoint
atomic batches concurrently, resumes checksum-verified parallel loads, binds
prepared primary keys, compiles DML plans concurrently, and retains compact
qualification evidence with artifact checksums.

The fresh 2026-08-20 Release qualification loaded 8,990,118 keys in 100.014
seconds. Three 32-terminal repetitions measured 54.900, 54.298, and 53.728 TPS
(54.304 aggregate, 2.1806% variance). All five p95 thresholds, synchronous
commit, consistency, online checkpoint, clean/unclean restart, checkpoint
restore, and disk-watermark rejection passed. This is TPC-C-like engineering
evidence, not official tpmC. The aggregate command is `make lite-m22`.
