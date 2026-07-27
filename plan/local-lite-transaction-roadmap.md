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
- Persistent rows use the `LLBR1` binary aligned row payload.
- SQLCI no longer directly executes supported local table queries against
  RocksDB.

The important storage limits are:

- `LocalLiteRocksDBStore` owns RocksDB handles per object instance.
- Table data operations reopen the table RocksDB database per `putRow()` and
  `scanRows()` call.
- `allocateRowId()` updates catalog `nextRowId` with a plain read-modify-write.
- `allocateRowId()` and `putRow()` are separate writes.
- There is no local transaction context, snapshot ownership, write set, commit,
  or rollback path.

This means the current implementation is appropriate for serial smoke tests,
but not yet for concurrent scans, concurrent writers, or explicit SQL
transactions.

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
operation protected by the local storage manager mutex. Because catalog
metadata and table rows still live in separate RocksDB databases, this phase
does not yet claim cross-process or crash-atomic multi-DB commit semantics.

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

Move closer to the original Trafodion HBase/TiKV-style key model.

Tasks:

- Add primary key metadata support for local-lite tables.
- Generate local row keys from compiler/executor key expressions when a table
  has a primary key.
- Keep internal row id allocation only for keyless heap-like local tables.
- Add duplicate-key detection for primary and unique keys.
- Add conflict diagnostics that map cleanly to SQL errors.

Validation:

- Concurrent inserts of the same primary key cannot silently overwrite each
  other.
- Primary-key scans use deterministic encoded keys.
- Keyless table inserts continue to work through the internal row id path.

### Phase 6: Optional TMF Integration Boundary

Only consider this phase after local-lite has stable local transaction behavior.

Tasks:

- Keep `LocalLiteTxnManager` as the only executor-to-storage transaction API.
- Add an implementation that can bind to Trafodion `ExTransaction` ids when a
  full TMF/DTM/RMS stack is available.
- Do not expose TMF-specific concepts directly inside local table scan/insert
  TCBs.

Validation:

- Existing local autocommit and explicit local transaction tests still pass.
- TMF-enabled builds can route transaction ids through the same facade.
- TMF-disabled local-lite builds do not require transaction service processes.

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

Phase 1 is now implemented for the current local-lite process model:

- `LocalLiteStorageManager` owns shared RocksDB catalog and table handles inside
  each linked local-lite module.
- `LocalLiteRocksDBStore` instances acquire and release the shared manager
  instead of directly owning a catalog DB handle.
- `putRow()` and `scanRows()` use cached table handles instead of reopening the
  same table RocksDB path per operation.
- Same-process row-id allocation is protected by the manager mutex.
- The RocksDB SQLCI smoke includes a self-join regression over one local table.

The next implementation step should be Phase 5:

1. Add primary key metadata support for local-lite tables.
2. Generate deterministic local row keys from compiler/executor key expressions
   when a table has a primary key.
3. Keep internal row id allocation only for keyless heap-like local tables.
4. Add duplicate-key diagnostics for primary and unique keys.
