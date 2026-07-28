# Local-Lite Transaction And Concurrency Roadmap

## Purpose

This document defines how local-lite should evolve from the current
single-process RocksDB smoke path into a storage layer that matches Trafodion's
executor transaction boundaries.

The immediate goal is not full TMF/DTM/RMS integration. Local-lite currently
does not start the Trafodion transaction service stack. The practical goal is to
introduce the same shape of ownership that Trafodion uses elsewhere:

- The executor owns statement and transaction boundaries.
- Storage receives an explicit operation context.
- Reads and writes go through executor TCBs.
- RocksDB is hidden behind a local-lite storage API.
- Local-lite can later replace the storage backend without changing executor
  TCB semantics.

## Current Baseline

Local-lite currently supports a narrow local table path:

- `CREATE TABLE` and `DROP TABLE` are routed through compiler DDL code and write
  local RocksDB catalog metadata.
- `INSERT INTO ... VALUES` executes through `LocalLiteHbaseInsertTcb`.
- `SELECT` executes through `LocalLiteHbaseScanTcb`.
- Executor scan can consume encoded `listOfGetRows()` row-key requests through
  `LocalLiteTxn::getRowByKey()` and falls back to full scan when no get-row
  request is present.
- Persistent rows use the `LLBR1` binary aligned row payload.
- SQLCI no longer directly executes supported local table queries against
  RocksDB.

The important storage limits are:

- `LocalLiteStorageManager` owns process-local shared RocksDB catalog and table
  handles.
- Keyless table rows still use catalog `nextRowId` and an internal 8-byte row
  key.
- Primary-key local tables store row data under deterministic binary row keys
  derived from the persisted `LLBR1` row payload.
- The explicit local transaction context is single-process and buffers pending
  writes in memory until `COMMIT WORK`.
- There is no RocksDB TransactionDB or cross-process transaction coordinator
  yet.

This means the current implementation is appropriate for serial smoke tests,
same-process scan reuse, and single-process explicit SQL transaction smoke, but
not yet for cross-process concurrent writers or crash-atomic multi-table commit.
Cross-process attempts to open the same `TRAF_LOCAL_STORE_DIR` are rejected by
RocksDB's DB lock and are now surfaced as an explicit local-lite process-boundary
diagnostic.

## Trafodion Transaction Model To Preserve

The existing Trafodion model has three useful boundaries for local-lite:

- `ExTransaction` represents the current executor transaction state and handles
  `BEGIN`, `COMMIT`, and `ROLLBACK`.
- HBase executor TCBs do not invent their own transaction semantics. They read
  the current transaction id from CLI context and pass it down to storage.
- Storage-specific code handles conflict detection, row locking, atomicity, and
  durability for that transaction context.

For local-lite, this means executor TCBs should not keep calling raw RocksDB
functions directly. They should call a local transaction facade that can run in
autocommit mode today and explicit transaction mode later.

## Target Architecture

The target local-lite storage stack should have these layers:

```text
SQL statement
  -> compiler-generated plan
  -> executor TCB
  -> LocalLiteTxn / LocalLiteTxnManager
  -> LocalLiteStorageManager
  -> shared RocksDB catalog/table handles
```

`LocalLiteStorageManager` should provide process-local ownership of RocksDB
database handles:

- one shared catalog handle per local store root;
- one shared table handle per table path;
- reference-counted or process-lifetime handle ownership;
- per-table mutexes for metadata and row-id critical sections;
- a clear close/shutdown path for tests.

`LocalLiteTxnManager` should provide the executor-facing transaction shape:

- create an autocommit transaction for statements without explicit SQL
  transaction state;
- bind to the current CLI transaction id when local explicit transactions are
  enabled;
- expose statement commit and rollback hooks;
- hide whether writes are immediate, batched, or buffered.

## Roadmap

### Phase 1: Shared RocksDB Handle Ownership

Status: implemented for process-local handle sharing and covered by the
RocksDB SQLCI smoke self-join regression.

Fix the current same-process lock problem first.

Tasks:

- Add `LocalLiteStorageManager`.
- Move catalog open/close ownership out of each `LocalLiteRocksDBStore`
  instance.
- Cache table handles by table path.
- Make `putRow()` and `scanRows()` use shared table handles.
- Add process-local mutexes for catalog metadata updates and table writes.
- Keep the public behavior unchanged for existing DDL, INSERT, and SELECT.

Validation:

- Existing local-lite runtime smoke passes.
- Existing RocksDB SQLCI smoke passes.
- A self-join over one local table no longer fails on RocksDB `LOCK`.
- Two scans over the same table in one query use executor scan TCBs and do not
  reopen the same RocksDB path incompatibly.

### Phase 2: Statement-Level Atomic Writes

Status: initial autocommit facade implemented for local table INSERT. Row-id
allocation and row persistence now go through one `LocalLiteTxn::insertRow()`
operation protected by the local storage manager mutex. The old public
`allocateRowId()` and direct `putRow()` store APIs have been removed, so local
executor writes cannot bypass the transaction facade. Because catalog metadata
and table rows still live in separate RocksDB databases, this phase does not yet
claim cross-process or crash-atomic multi-DB commit semantics.
Same-process concurrent write coverage now exercises multiple writer threads
plus an overlapping scanner through the local transaction facade and validates
contiguous, duplicate-free row ids.

Make every autocommit statement atomic at the local storage layer.

Tasks:

- Introduce `LocalLiteTxn` in autocommit mode.
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
- Row id allocation does not duplicate under same-process concurrent inserts.
- Failed insert expression or constraint evaluation does not advance persisted
  row state for that row.

### Phase 3: Snapshot-Based Statement Reads

Status: initial scan facade implemented. Local table scans now call
`LocalLiteTxn::scanRows()`, and RocksDB iterators are bound to a snapshot for
the duration of each scan materialization. A statement-wide snapshot shared by
multiple scan TCBs still requires a future local transaction context.

Give scans a stable statement snapshot.

Tasks:

- Add snapshot acquisition to `LocalLiteTxn` for read statements.
- Make `LocalLiteHbaseScanTcb` scan with the statement snapshot.
- Release snapshots when the statement finishes, errors, or is cancelled.
- Preserve current binary row materialization and projection mapping.

Validation:

- A scan sees a stable view while another same-process writer inserts rows.
- Repeated scans inside one statement observe the same snapshot when required
  by executor semantics.
- Predicate and projection tests continue to pass for all supported scalar
  types.

### Phase 4: Explicit Local Transactions

Status: initial local transaction context implemented for single-process
local-lite SQLCI. `BEGIN WORK` creates a pending write set, local table INSERTs
buffer writes in that set, scans read base RocksDB rows plus own pending writes,
`ROLLBACK WORK` discards pending rows, and `COMMIT WORK` persists them through
the existing local insert path. This phase does not yet provide RocksDB
TransactionDB semantics or all-table atomic commit across failures.

Add local transaction behavior for `BEGIN`, `COMMIT`, and `ROLLBACK` without
starting TMF/DTM/RMS.

Tasks:

- Teach local-lite transaction statements to create and finish a
  `LocalLiteTxnContext` in CLI context.
- Route local table writes into the current local transaction context when one
  exists.
- Buffer writes in a local write set or use RocksDB TransactionDB.
- Make `COMMIT` atomically publish the transaction write set.
- Make `ROLLBACK` discard uncommitted writes.
- Make scans read from the transaction snapshot plus own writes.

Validation:

- `BEGIN; INSERT ...; ROLLBACK; SELECT ...` does not show rolled-back rows.
- `BEGIN; INSERT ...; COMMIT; SELECT ...` shows committed rows.
- A transaction can read its own writes.
- Autocommit behavior remains unchanged outside explicit transactions.

### Phase 5: Deterministic Row Keys And Conflict Detection

Status: initial primary-key storage support implemented. Local-lite DDL now
accepts PRIMARY KEY and UNIQUE, persists key column ordinals in `LLT3` table
metadata, and rejects RI/CHECK constraints. INSERTs into primary-key tables
build a deterministic `P`-prefixed RocksDB row key from the binary aligned
`LLBR1` payload. UNIQUE constraints use `U`-prefixed secondary uniqueness
records in the same RocksDB table. Duplicate primary and unique keys are
rejected in committed data and inside the current pending local transaction
write set. Keyless tables continue to use the existing internal row id path.
The local-lite executor scan TCB can now consume encoded get-row requests from
`listOfGetRows()` through `LocalLiteTxn::getRowByKey()`, including read-own-write
lookups inside the pending local transaction write set. Pre-code can now rewrite
constant primary-key equality search keys to deterministic `P`-prefixed
local-lite get-row keys. Binary NUMERIC, DECIMAL, and BigNum NUMERIC key
literals can now be encoded into deterministic get-row keys, including negative
predicate forms that the compiler represents as constant expressions rather than
plain `ConstValue`s. UNIQUE-key equality predicates can now be mapped to
deterministic `U`-prefixed get-row keys when the predicate supplies all columns
of one unique key. Local-lite NATable synthesis now exposes primary-key and
UNIQUE-key metadata to the optimizer. UNIQUE keys are represented as logical
unique access paths that keep the physical scan name on the base local table;
the executor resolves `U` records back to the persisted base `LLBR1` row.
Local-lite DML also skips generic secondary-index maintenance because the
storage layer maintains `U` uniqueness records in the same RocksDB table.
UNIQUE get-row rewrite removes only predicates covered by the logical unique key
and leaves residual predicates for executor evaluation. Unsupported literal
types and non-key predicates still fall back to full executor scan. SQLCI smoke
now enables a test-only executor trace to assert that integer primary-key,
NUMERIC/DECIMAL/BigNum primary-key, and UNIQUE-key equality use the get-row path
while non-key predicates use full scan fallback. The same smoke also checks
EXPLAIN output for primary-key and UNIQUE-key equality subset scans, key column
metadata, single-probe access, and residual executor predicates.

Move closer to the original Trafodion HBase/TiKV-style key model.

Tasks:

- Add primary key metadata support for local-lite tables.
- Generate local row keys from compiler/executor key expressions when a table
  has a primary key.
- Keep internal row id allocation only for keyless heap-like local tables.
- Add duplicate-key detection for primary keys.
- Add conflict diagnostics that map cleanly to SQL errors.
- Add unique-key metadata and duplicate detection after primary-key storage is
  stable.
- Teach local-lite scan/get TCBs to consume optimized key access before exposing
  local-lite primary keys as optimizer-visible key metadata.
- Expose local-lite UNIQUE keys as optimizer-visible logical access paths while
  keeping executor scan storage on the base local table.

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

### Phase 6: Optional TMF Integration Boundary

Status: initial executor-bound transaction facade implemented. `LocalLiteTxnManager`
now exposes executor-bound begin/commit/rollback entry points and records both a
local transaction id and the executor transaction id token for the active local
transaction. The local-lite transaction TCB path calls these facade methods.
When a real `ExTransaction` id is available the facade can bind to it; in the
current TMF-disabled local-lite path the executor transaction object identity is
used as a local binding token. Scan and insert TCBs still talk only to
`LocalLiteTxn` and do not receive TMF-specific state directly.

Only consider this phase after local-lite has stable local transaction behavior.

Tasks:

- Keep `LocalLiteTxnManager` as the only executor-to-storage transaction API.
- Add an implementation that can bind to Trafodion `ExTransaction` ids when a
  full TMF/DTM/RMS stack is available.
- Do not expose TMF-specific concepts directly inside local table scan/insert
  TCBs.
- Add a real TMF-backed implementation after a full service stack is available.

Validation:

- Existing local autocommit and explicit local transaction tests still pass.
- TMF-enabled builds can route transaction ids through the same facade.
- TMF-disabled local-lite builds do not require transaction service processes.
- Runtime structure checks assert that local-lite transaction statements call
  executor-bound facade methods.

## Design Decisions

### Do Not Start With Full TMF

Starting with full TMF would make local-lite depend on the service stack it is
designed to avoid. The first useful milestone is a local transaction facade
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

The persisted `LLBR1` row payload should remain the table row value. Transaction
metadata, locks, versions, or write intents should live in storage keys,
side-records, RocksDB metadata, or the transaction manager layer.

## Immediate Next Implementation Step

The storage API audit removed the legacy direct row-id allocation and row-put
entry points from the public local store surface. Same-process writer/scan
runtime coverage now guards the shared handle and row-id allocation path.
Cross-process shared-store attempts now receive a local-lite diagnostic that
names the `TRAF_LOCAL_STORE_DIR` boundary. The next implementation step should
return to executor expression coverage:

1. Broaden executor expression coverage for local table scan predicates and
   insert value expressions beyond the current smoke cases.
2. Audit any newly added local-lite RocksDB open paths and confirm executor scan
   and insert TCBs continue to use process-local shared handles.
3. Keep the current SQLCI trace and EXPLAIN smoke as guards for get-row versus
   full-scan behavior while changing storage ownership.
