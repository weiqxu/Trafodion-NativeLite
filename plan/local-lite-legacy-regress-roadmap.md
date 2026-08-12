# Local-Lite Legacy Regress Roadmap

## Purpose

Milestones 0 through 10 in this roadmap track the work required to run the
portable, non-Hive, and non-HBase portions of the legacy suites under
`core/sql/regress` against the standalone local-lite SQLCI and RocksDB store.
Milestones 11 and 12 extend that validated SQL surface into the session,
service, transaction, recovery, and storage boundaries required for a
production-oriented single-node database.

The legacy suites are not a drop-in replacement for the native
`core/sql/regress/localLite` lane. They mix portable SQL semantics with HBase
physical access, Trafodion service-stack assumptions, authorization, UDRs,
shell helpers, and EXPECTED output tied to the original storage layout. The
first milestone therefore establishes an auditable inventory and runner before
new SQL surface is added.

## Scope And Acceptance Rules

The initial inventory covers primary TEST inputs with matching EXPECTED files
in these suites:

- `charsets`
- `compGeneral`
- `core`
- `executor`
- `fullstack2`
- `privs1`
- `privs2`
- `seabase`
- `udr`

Exclusion is based on a test or section's behavior, not only its directory.
HBase-dependent sections inside `seabase`, `executor`, or another suite remain
out of the portable target even though there is no separate `hbase` directory
in this checkout.

Every included test or section must have:

- a manifest entry with a classification, disposition, milestone, and blocker;
- an isolated `TRAF_LOCAL_STORE_DIR` when executed;
- retained RAW, LOG, DIFF, exit-status, and first-diagnostic evidence;
- no unexpected skip or broad output filter that hides a semantic failure;
- native local-lite regress, runtime, and RocksDB SQLCI smoke remaining green.

## Milestone 0: Inventory, Adapter, And Baseline

Status: complete. Milestone 0 inventories 122 primary TEST inputs
(119 numeric TEST names plus `TESTRTS`, `TESTTOK`, and `TESTTOK2`), validates
manifest completeness and safety dispositions, provides an isolated RocksDB
adapter, and records RAW/LOG/EXPECTED/DIFF/RESULT evidence. The first baseline
probe, `charsets/TEST012`, exits SQLCI normally and identifies diagnostic `3242`
at its DEFAULT column definition as the first blocker; SHOWDDL metadata,
UPDATE/DELETE, and UCS2/long-character work remain later dependencies in that
same TEST. The 14 initial `charsets` review entries are now classified: one is
runnable (`TEST316`) and thirteen have concrete roadmap blockers. The remaining
manual test/section review was completed suite by suite.
The nine safe `compGeneral` review entries are also classified; `TEST045` was
split into a portable body and an unsafe cleanup section after the latter
reproducibly terminated SQLCI with status 139. There are now 58 M0 manifest
entries remaining, including three `compGeneral` shell/abort cases that still
need section-level safety review.

The 17 initial `core` review entries are now classified: `TEST163` is runnable,
14 have concrete roadmap blockers, and `TEST000`/`TEST001` are unsafe because
of an external OBEY dependency and a reproducible SQLCI status-139 failure.
The external OBEY safety guard now covers any macro-based path. There are 41
M0 manifest entries remaining.

The ten initial safe `executor` review entries are now classified: seven are
blocked, `TEST013` and `TEST107` are unsafe after status-139 failures, and
`TEST140` is excluded because its section inseparably combines Hive load and
HBase physical options. Repeated failures from `cleanup obsolete volatile
tables` added a dedicated safety guard and also moved `seabase/TEST010` out of
M0. There are 30 M0 manifest entries remaining.

The eleven remaining safe `seabase` review entries are now classified. HBase
tests are excluded, `TEST012` is split into HBase and portable schema selectors,
and the portable sequence, identity, constraint, and datetime tests have
concrete M4/M6 blockers. No `needs-review` disposition remains. The 19 M0
entries still open are unsafe whole-test entries that require section-level
safety review rather than direct execution.

The first unsafe section-review batch covers `compGeneral/TEST004`, `TEST006`,
`TEST042`, and `core/TEST038`, `TEST116`, `TEST131`. SQL-only bodies were split
from shell-driven sections; the safe probes captured concrete M4/M5/M7 blockers,
one optimizer prepare abort (status 134), and one bounded 30 second baseline.
The HBase-centric `TEST116` is excluded and the multi-user authorization
`TEST131` moves to M8. The adapter now materializes section bodies in selector
order so hyphenated section names and repeated cleanup sections work safely.
There are 13 M0 manifest entries remaining.

The final batch classifies all remaining `executor` and `seabase` entries.
Portable SQL bodies from `executor/TEST063`, `executor/TEST122`,
`seabase/TEST003`, and `seabase/TEST004` have bounded baseline evidence;
Hive/HBase cases are explicitly excluded, and external compiler, AQR, cancel,
LOB file, and shell-driven cache tests retain concrete unsafe reasons. The M0
manifest count is now zero, so Milestone 0 is complete.

Deliverables:

1. A versioned `localLiteLegacy/manifest.tsv` that accounts for every primary
   legacy TEST input and supports section-level rows.
2. An audit script that detects missing/unknown manifest entries, validates the
   vocabulary, identifies unsafe helpers, and reports suite/milestone totals.
3. A local-lite legacy adapter that executes only allowlisted entries by
   default, uses an isolated store and run directory, and refuses unsafe shell
   or abort directives.
4. A repeatable baseline report containing the first SQL diagnostic and diff
   artifact for every probed entry.

Milestone 0 does not implement missing SQL features and does not change legacy
TEST or EXPECTED files. Static keyword classification is only a candidate;
mixed tests require manual section review before their disposition is final.

Completion criteria:

- every primary TEST with a matching EXPECTED file is represented;
- no manifest entry points at a missing suite or TEST;
- every exclusion has a concrete reason;
- every unsafe helper is rejected before SQLCI starts;
- the baseline can be regenerated without modifying the source suites;
- report totals are stable and suitable for measuring later milestones.

## Milestone 1: UPDATE

Status: completed in the working tree. Native regression coverage is in
`localLite/TEST026`; the full local-lite suite passes 26/26. Re-probing the two
legacy tests previously blocked first by UPDATE confirms that
`charsets/TEST003` now reaches DELETE and `executor/TEST101` completes its
six-row NULL-scale UPDATE before later suite-convergence differences.

Implement local update generation/execution, transaction pending mutations,
PK/UNIQUE key changes, index-ready mutation hooks, read-own-update, statement
atomicity, rollback, diagnostics, and row counts.

Completion requires single/multi-row UPDATE, expression and NULL assignment,
key changes, late conflicts, autocommit, and explicit transaction coverage.

## Milestone 2: DELETE, UPSERT, And MERGE

Status: completed in the working tree. DELETE, primary-key UPSERT, and MERGE
have native coverage in `localLite/TEST027`, `TEST028`, and `TEST029`. UPSERT
covers insert/update selection, multi-row VALUES, INSERT/DELETE overlay,
explicit commit/rollback, UNIQUE-key replacement/reuse, and UPSERT SELECT.
MERGE covers matched update, not-matched insert, conditional matched actions,
read-own-write in an explicit transaction, rollback, and statement rollback on
a late UNIQUE conflict. Per-table transaction publication validates the final
row/key image and applies UPDATE/DELETE/INSERT through one RocksDB WriteBatch.

The executor uses RocksDB rows exclusively. Historical compiler/executor class
names containing `Hbase` remain node names, not a dependency on an HBase client,
transaction manager, or HFile path. Accordingly, the HBase/HFile-specific
UPSERT USING LOAD path remains intentionally unsupported. Cross-table commit is
also still intentionally not atomic.

Implement pending deletes, UNIQUE record removal, DELETE visibility, UPSERT
insert/update selection, MERGE matched/not-matched paths, statement atomicity,
and transaction overlay behavior.

## Milestone 3: Secondary Indexes

Status: complete. The RocksDB-only implementation persists unique and
non-unique index definitions in the local catalog, supports `CREATE INDEX` and
`DROP INDEX`, records composite key order, backfills physical index records,
and maintains them atomically with INSERT, UPDATE, DELETE, UPSERT, MERGE, and
transaction commit. It validates object and column conflicts, rejects duplicate
keys while building or mutating a unique index, and removes index registrations
and physical records on DROP. Full-key equality predicates on unique and
non-unique indexes are compiled into RocksDB index-prefix scans with residual
predicate validation; explicit transactions safely fall back to visible-row
filtering while physical index changes are pending. Native coverage is in
`localLite/TEST030` through `localLite/TEST034`. Composite leading-key prefixes
and first-key range predicates now use RocksDB scans with residual validation.
New integer, fixed-width NUMERIC, and VARCHAR indexes use versioned
order-preserving keys plus half-open lower/upper RocksDB bounds, including DESC
inversion; legacy and unsupported-type encodings safely retain candidate scans.
The ordered encoding also covers fixed-width CHAR, REAL/FLOAT(1-22), and
FLOAT(23-54)/DOUBLE values; floating signed zero is canonicalized so `-0` and
`+0` remain one UNIQUE key.
Version 3 ordered keys persist NULL components, allow multiple NULL values in a
UNIQUE index, and compile leading-key `IS NULL` predicates into RocksDB prefix
or bounded scans; transactional mutations still use visible-row filtering until
commit. Version 4 index values contain the base row key and encoded row, enabling
index-only scans without a RocksDB base-row lookup while retaining transparent
fallback for legacy index values. Optimizer metadata now exposes explicit
secondary indexes, key direction, uniqueness, primary-key suffixes, and covering
columns; the optimizer can cost and select them, and the executor resolves the
selected index back to its owning RocksDB table. Generator-side candidate ranking
prefers longer prefixes and exact unique matches when several usable indexes are
available. Ordered keys cover every type currently accepted by local-lite table
DDL: integer and floating numerics, fixed and wide NUMERIC/DECIMAL, CHAR/VARCHAR,
and DATE/TIME/TIMESTAMP. Nullable composite prefixes support consecutive
`IS NULL` components and range predicates following NULL/equality components.
EXPLAIN identifies RocksDB access and secondary index-only scans, while scan
tracing reports the selected index and covering/base-lookup counts. Native
coverage is in `localLite/TEST030` through `localLite/TEST035`; the RocksDB SQLCI
smoke also asserts optimizer selection, index-only execution, EXPLAIN visibility,
and absence of HBase/Hadoop/JVM runtime linkage.

Implement index DDL and metadata, unique/non-unique key encoding, mutation
maintenance, lookup/range/index-only scans, optimizer exposure, residual
predicates, and EXPLAIN coverage.

## Milestone 4: Catalog DDL And Constraints

Status: complete in the working tree. The local catalog now persists schemas,
views, synonyms, sequences, table defaults, CHECK and RESTRICT/NO ACTION RI
constraints, identity allocation state, triggers, dependency metadata, and
ALTER/TRUNCATE changes in RocksDB. Metadata updates use replacement writes and
invalidate compiler table metadata; row/key migrations are validated before
publication. Native coverage is in `localLite/TEST009`, `TEST021`,
`TEST036`, and `TEST037`, and the RocksDB SQLCI smoke validates positive view,
ALTER, TRUNCATE, DEFAULT, and CHECK behavior. The intentionally bounded
surface is explicit: RI CASCADE actions and computed system columns remain
unsupported, and HBase/HFile execution is not used.

Implement schemas, views, ALTER/TRUNCATE, DEFAULT, CHECK, RI, sequences,
identity/generated columns, synonyms, triggers, object dependencies, cache
invalidation, and failure-atomic metadata changes.

## Milestone 5: Metadata And Statistics

Status: complete for the RocksDB-only local-lite surface in the working tree.
`LocalLiteSqlTable_process` now serves deterministic SHOWDDL, computes and
persists table/column row and NULL statistics, and executes UPDATE STATISTICS
without arkcmp/HBase.  The catalog has a versioned statistics record and
RocksDB row null-bitmap inspection; SHOWSTATS reads the persisted record (or
collects it on first use). Successful INSERT/UPDATE/DELETE and transaction
commit invalidate the table record, so the next SHOWSTATS observes the current
RocksDB rows. Native coverage is in `localLite/TEST038`, including those
post-DML refreshes, and the full native suite is 38/38. The intentionally
excluded surface is direct legacy SQL against physical `_MD_` tables and full
value histograms/UEC estimation; local-lite metadata is exposed through
SHOWDDL/SHOWSTATS and all optimizer/explain behavior remains RocksDB-native.

## Milestone 6: Character Sets And Data Types

Status: complete for the RocksDB-only local-lite surface in the working tree.
The catalog now preserves UTF8/UCS2 character metadata and byte capacity,
retains BINARY/VARBINARY instead of normalizing them to character strings, and
maps BOOLEAN, INTERVAL, and LONG VARCHAR into optimizer descriptors.  The
canonical RocksDB row codec stores and projects these types, handles NULL and
bounded multibyte values, and includes BOOLEAN/INTERVAL/BINARY/VARBINARY in
secondary-key encoding.  Native coverage is in `localLite/TEST039`; together
with the existing suite the full local-lite lane passes 39/39.  The test also
exercises UTF8/UCS2 values, interval projection, binary storage, a composite
RocksDB index, BOOLEAN predicates, assignment, and index lifecycle.

The deliberate boundary remains explicit: BLOB/CLOB/ARRAY and HBase/HDFS/LOB
backing are rejected with the existing local-lite diagnostic; non-default
collations and character translations outside ISO88591/UTF8/UCS2 are not
claimed by this milestone.  All supported data is persisted in RocksDB tables;
no HBase client or HFile path is used.

## Milestone 7: Advanced Executor Coverage

Status: complete for the RocksDB-only, single-process local-lite surface in the
working tree.  The local-lite regress runner enables SQLCI cursor execution;
updatable `FOR UPDATE` cursors now have native coverage for FETCH, positioned
UPDATE, positioned DELETE, and close/teardown.  The normal executor path covers
multi-row VALUES/insert-select tuple flow, window functions (ROW_NUMBER, RANK,
DENSE_RANK, LAG, LEAD, and running aggregates), grouping/sorting, and executor
statistics/lifecycle through the existing queue and BMO implementations.
RocksDB scan row handles are cleared on cancellation and TCB teardown, and
local-lite startup assigns an isolated scratch directory under
`TRAF_LOCAL_STORE_DIR` for sort/hash overflow files.  Native coverage is in
`localLite/TEST040`; the full native lane passes 40/40 and the RocksDB SQLCI
smoke remains green.

The deliberate boundary is explicit: standalone SQLCI does not expose embedded
host-language rowset descriptors, local-lite tables have no physical partition
or ESP fan-out, and execution is single-process with RocksDB tables only.
HBase/HDFS/HFile access and service-stack cancellation are not used.

## Milestone 8: Authorization

Status: complete for the RocksDB-only local-lite surface in the working tree.
The catalog now persists users, roles, role membership, table ownership,
table-level SELECT/INSERT/UPDATE/DELETE/REFERENCES/USAGE privileges, grant
changes, and an authorization generation counter.  SQLCI `-u` and
`SET SESSION AUTHORIZATION` resolve identities from that catalog and publish
the effective/session identity to `CURRENT_USER`, `SESSION_USER`, and `USER`.
Role membership is transitive, owners and `DB__ROOT` retain administrative
control, and GRANT/REVOKE changes are checked before compilation and again at
prepared-statement execution so a cached plan cannot bypass a revoke.  Views
use their persisted owner as the local-lite definer boundary; switching the
session identity provides the invoker boundary.  Native coverage is in
`localLite/TEST041`, and the full local-lite lane passes 41/41.

The deliberate boundary is explicit: this milestone has no LDAP/password
authentication, external security service, column-level privilege metadata,
or UDR definer-rights implementation (UDR is M9).  Authorization metadata,
table data, and all ownership checks are RocksDB catalog operations only; no
HBase/HDFS/HFile path is used.

## Milestone 9: UDR

Status: complete for the bounded RocksDB-only local-lite surface in the
working tree.  SQLCI now handles UDR DDL and invocation before the MX compiler
path, so no `_MD_` metadata tables, HBase/HDFS/HFile access, UDR server, or
Trafodion service stack is required.  Library and routine definitions are
versioned records in the RocksDB catalog; dropping a library removes its
dependent routines, and routine DDL is restricted to `DB__ROOT`.

The native adapter loads `dlopen`/`dlsym` libraries (including deterministic
in-process `builtin` routines), runs the SQL UDR INITIAL/NORMAL/FINAL lifecycle,
preserves NULL indicators and state/error buffers, closes external handles on
all paths, and reports the UDR message on failure.  The Java adapter invokes a
configured class through the local JDK command path and converts its result
row back to SQLCI values.  Scalar functions, procedures with IN/OUT
parameters, one-row table-mapping functions, and `PREPARE`/`EXECUTE` are
covered by `localLite/TEST042`; native and Java adapters share the same
RocksDB metadata and invocation path.  Result-set output is represented as a
deterministic SQLCI row, and invocation has no hidden table mutation, so
COMMIT/ROLLBACK and isolation remain the caller's local-lite transaction
boundary rather than an HBase side effect.

The deliberate boundary is explicit: the portable adapter currently supports
the SQL UDR ABI for at most two INT inputs and two INT outputs, emits one row
for a table mapping/result-set call, and does not embed host rowset
descriptors, a JVM in the SQLCI process, or transactional SQL access from the
UDR body.  Those capabilities belong to a future service-backed milestone;
they are not silently claimed by the RocksDB-only lane.

## Milestone 10: Suite Convergence

Status: the bounded convergence gate is implemented, but the full legacy
portable suite is not yet complete.  `scripts/test-local-lite-legacy-convergence.sh`
now validates the manifest, runs every currently allowlisted portable entry,
and reruns the native `TEST001-TEST043` lane.  The current allowlist is
`charsets/TEST003`, `charsets/TEST316`, `core/TEST018`, `core/TEST163`,
`executor/TEST014`, and `executor/TEST101`, all passing; the native lane remains
43/43.
This was revalidated from a clean local-lite build on 2026-08-12; the six
allowlisted legacy cases and all 43 native cases passed with exact normalized
EXPECTED output.
`charsets/TEST003` validates UCS2 column storage, literal assignment,
UPDATE/DELETE, supported translation, and the expected rejection of unsupported
character-set and translation names.  `executor/TEST014` validates CTAS,
volatile CTAS, STORE BY compatibility, INVOKE, and CTAS diagnostics on ordinary
RocksDB tables.  The LocalLite EXPECTED files record the local SQLCI
diagnostic/DDL display contract without changing legacy source files.  This
prevents a legacy expected-file mismatch or a skipped entry from being reported
as M10 success.
`core/TEST018` covers secondary-index maintenance across INSERT, UPDATE, DELETE,
and transaction rollback on RocksDB tables.

M10 is split into six explicit gates so a suite-convergence result can be
traced back to a storage or executor capability:

| Gate | Status | RocksDB-only deliverable | Required evidence |
| --- | --- | --- | --- |
| M10A | bounded complete | CTAS and volatile CTAS use the ordinary local DDL path; logical PARTITION/DIVISION/STORE BY/table-attribute hints are accepted as non-physical hints; view mutability and WITH CHECK OPTION metadata are persisted and exposed to the binder | `executor/TEST014`, native `TEST009`; `core/TEST029` remains blocked because its scalar-subquery NULL update aborts after binding and needs a compiler/optimizer fix before promotion |
| M10B | complete | RocksDB row/null statistics are persisted, UEC is computed from non-NULL encoded values, and DML/transaction publication invalidates stale statistics | native `TEST038` |
| M10C | complete | Primary/UNIQUE DML, secondary-index maintenance, ordered/range/index-only access, and rollback remain atomic in the local store | `core/TEST018` plus native `TEST026`-`TEST035` |
| M10D | complete | UTF8/UCS2, binary types, BOOLEAN/INTERVAL, and supported character translation use the canonical RocksDB row/key codec | `charsets/TEST003`, `charsets/TEST316`, native `TEST039` |
| M10E | complete | Cursor, window, grouping, sorting, cancellation cleanup, and local scratch-file lifecycle run through the single-process executor | native `TEST040` |
| M10F | complete | Catalog-backed identity/role/privilege checks and the bounded native/Java UDR adapters run without a service stack | native `TEST041` and `TEST042` |

The phase gate is executable with `make local-lite-m10`; it checks the
allowlisted legacy rows, the native 43-test lane, and every M10A-M10F evidence
case.  “Complete” above means complete for the declared RocksDB-only surface;
it does not promote physical HBase/Hive tests, shell-driven multi-session tests,
or a test that still crashes in the compiler.  No HBase table or HBase metadata
implementation is part of these gates.

The remaining legacy inventory is explicit rather than silently filtered: 57
entries are still blocked by unsupported SQL/metadata or legacy output
contracts, 50 are unsafe because they require shell/service-stack behavior,
and 21 are excluded for HBase/Hive/physical-storage behavior.  In particular,
`executor/TEST101` now reaches its UPDATE body and pins the local diagnostic/
CQD rendering in its LocalLite EXPECTED; the remaining blocked charset entries still require
unsupported character-set paths or later DDL/statistics work.  The catalog now
persists RocksDB-native
logical metadata rows under the `md|OBJECTS|`, `md|TABLES|`, `md|COLUMNS|`, and
`md|KEYS|` prefixes and maintains them on table/index create, alter, and drop.
The compiler now resolves the supported `_MD_` tables to synthetic descriptors,
and the executor builds OBJECTS/TABLES/COLUMNS/KEYS/INDEXES rows directly from
the RocksDB catalog.  The focused check is `make local-lite-metadata`; it
validates table/index creation, metadata scans, and cleanup.  The compatibility
descriptor exposes metadata scalar fields as text because the legacy metadata
numeric/short-CHAR projection path is not available in the local executor.
No HBase implementation will be added; remaining work must be implemented
against RocksDB tables or retained as a concrete exclusion.

### RocksDB metadata layout

The local catalog uses ordered logical metadata keys without copying HBase
cells.  User data remains in the existing per-object RocksDB databases;
the catalog RocksDB stores ordered metadata keys:

```text
md|OBJECTS|<hex-catalog>|<hex-schema>|<hex-name>|<type>
md|TABLES|<object-uid>
md|COLUMNS|<object-uid>|<ordinal>
md|KEYS|<object-uid>|<key-id>|<sequence>
```

Values use the length-delimited `LLMD1` format.  The public
`LocalLiteRocksDBStore::scanMetadataRows` API exposes a metadata table as an
ordered key/value range for the upcoming SQLCI/compiler `_MD_` resolver.

Run and converge the portable sections in this order:

1. `charsets`
2. `core`
3. `executor`
4. `compGeneral`
5. portable `seabase` sections
6. `fullstack2`
7. `privs1`
8. `privs2`
9. `udr`

Completion means all included sections pass, all excluded sections retain an
explicit reason, there are no unexpected diffs, and the native local-lite lane
continues to pass in full.

## Milestone Status Summary

The completion labels below are scoped.  A completed M1-M9 milestone means its
declared RocksDB-only, single-process local-lite surface is implemented and has
the cited focused coverage; it does not imply that the full legacy suite or a
production multi-session database is complete.

| Milestone | Status | Current evidence and boundary |
| --- | --- | --- |
| M1 UPDATE | Complete | Native `TEST026`; UPDATE generation/execution, key changes, statement atomicity, explicit transaction behavior, and diagnostics are covered. |
| M2 DELETE, UPSERT, MERGE | Complete | Native `TEST027`-`TEST029`; per-table publication is atomic, but cross-table commit is not. |
| M3 secondary indexes | Complete | Native `TEST030`-`TEST035` plus RocksDB SQLCI smoke; equality, prefix, range, covering/index-only access, uniqueness, and DML maintenance are covered. |
| M4 catalog DDL and constraints | Complete for the declared local surface | Native `TEST009`, `TEST021`, `TEST036`, `TEST037`; later metadata/DDL compatibility work and `TEST043` extend this surface. RI CASCADE and computed system columns remain outside the boundary. |
| M5 metadata and statistics | Complete for the declared local surface | Native `TEST038`; SHOWDDL/SHOWSTATS and persisted row/NULL statistics are covered. Full histograms and unrestricted legacy physical `_MD_` behavior are not claimed. |
| M6 character sets and data types | Complete for the declared local surface | Native `TEST039`; ISO88591/UTF8/UCS2, binary types, BOOLEAN, INTERVAL, and LONG VARCHAR are covered. LOB/ARRAY and broader collation/translation behavior remain outside the boundary. |
| M7 advanced executor | Complete for the single-process surface | Native `TEST040`; cursors, windows, grouping, sorting, cancellation cleanup, and local scratch lifecycle are covered. There is no ESP fan-out or remote multi-session runtime. |
| M8 authorization | Complete for the local catalog surface | Native `TEST041`; users, roles, ownership, privileges, revoke checks, and view owner/invoker boundaries are covered. Password authentication and an external identity service are not. |
| M9 UDR | Complete for the bounded adapter surface | Native `TEST042`; versioned routine metadata and bounded native/Java invocation are covered. This is not the full UDR server or host-rowset surface. |
| M10 suite convergence | Bounded complete; full portable legacy convergence remains incomplete | `make local-lite-m10` covers the six allowlisted legacy entries and native `TEST001`-`TEST043`. The remaining inventory is still classified as blocked, unsafe/service-stack-dependent, or excluded physical HBase/Hive behavior. |
| M11 sessionized runtime and standalone server | Planned | Not started. This is the next productization milestone. |
| M12 transactional storage and recovery | Planned | Not started. It begins after the M11 session/server boundary is usable. |

## Milestone 11: Sessionized Runtime And Standalone Server

Status: planned; not started.  M11 is the next productization milestone.  Its
goal is to move local-lite from a single SQLCI process into one long-running
database service that owns the embedded store and safely hosts multiple client
sessions.  It does not select or replace the storage engine.

### M11A: Session-Owned Transaction Context

- Replace the process-global `LocalLiteTxnState` singleton with a transaction
  context owned by each `ContextCli` session.
- Keep `LocalLiteStorageManager` process-owned so one server process owns and
  shares the catalog/table handles.
- Resolve the current transaction context from executor statement/session
  state instead of implicit static access.
- Release statement snapshots and roll back an active transaction when a
  session disconnects, is cancelled, or is destroyed.
- Preserve the current `LocalLiteTxn` executor-facing API boundary so scan and
  DML TCB semantics do not depend on a future wire protocol.

M11A completion requires two independent CLI contexts in one process to begin
transactions concurrently, isolate uncommitted writes, commit or roll back
independently, resolve same-key conflicts deterministically, and clean up after
disconnect/cancel without poisoning the other session.

### M11B: Standalone NativeLite Server

- Add a long-running `nativelite-server` process that exclusively opens the
  configured local store.
- Create and destroy one `ContextCli` session per client connection.
- Provide prepare, execute, fetch, close, transaction, cancellation, health,
  and graceful-shutdown paths over an initial loopback/Unix-socket transport.
- Keep connection handling separate from compiler/executor/storage semantics so
  the initial transport can be replaced by the selected product protocol.
- Add a real two-client end-to-end test and a restart/persistence test.

M11B completion requires two clients to perform overlapping transactions
through the server, observe the declared isolation boundary, disconnect without
leaking transaction state, and read committed data after a clean or unclean
server restart.

### M11C: Product Client Protocol

- Time-box a choice between a reduced Trafodion client path and a standalone
  protocol such as PostgreSQL wire compatibility; do not restore DCS,
  ZooKeeper, Monitor, or the full service stack by default.
- Support authentication startup, statement lifecycle, result metadata/rows,
  transaction status, cancellation, and stable SQLSTATE mapping.
- Validate with a real JDBC or ODBC client.  An in-process CLI call is not
  end-to-end protocol evidence.

M11 completion does not claim production readiness.  It establishes the
multi-session service boundary needed for M12 durability, recovery, backup,
resource governance, and later security hardening.

## Milestone 12: Transactional Storage And Recovery

Status: planned; not started.  M12 makes the M11 service safe for durable
single-node transactional use.  Storage-engine selection is an M12 decision,
not a prerequisite baked into the executor.

### M12A: Storage Contract And Atomic Transaction Domain

- Introduce backend-neutral `StorageEngine`, `StorageSession`, `StorageTxn`,
  and streaming `StorageCursor` contracts.
- Put catalog metadata, base rows, primary/UNIQUE records, and secondary-index
  maintenance inside one transaction domain.
- Replace whole-table materialization at the storage API with bounded streaming
  range cursors.
- Define isolation, conflict, retryable error, cancellation, and synchronous
  durability contracts before selecting the backend.
- Preserve versioned row/key/catalog formats or provide an explicit migration.

### M12B: Backend Selection And Implementation

- Evaluate at least the single-database RocksDB TransactionDB layout and one
  transactional embedded alternative against the same correctness, recovery,
  and workload tests.
- Select the backend using transaction correctness and crash recovery first,
  p95/p99 stability and operational behavior second, and peak throughput after
  those gates.
- Do not map a successful point-get/scan prototype to production readiness.

### M12C: Recovery, Backup, Migration, And Operations

- Add synchronous commit/WAL policy, checkpoint, integrity verification,
  consistent backup, restore, and on-disk format-version checks.
- Add migration from the current per-table RocksDB layout with object, row,
  key/index, and checksum verification plus a documented rollback window.
- Inject process kill, restart, disk-full, interrupted checkpoint/backup,
  commit-boundary, and metadata/data publication failures.
- Add storage and transaction metrics, disk-watermark protection, and a
  repeatable restore drill.

M12 completion requires crash-atomic catalog/data and multi-table commits, one
declared cross-table transaction snapshot, no visibility of aborted writes,
successful recovery after every supported fault point, and a backup restored
into a fresh store that passes metadata, row, key/index, and SQL validation.
It still does not claim node-level high availability or distributed execution.
