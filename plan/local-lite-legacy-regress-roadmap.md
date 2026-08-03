# Local-Lite Legacy Regress Roadmap

## Purpose

This roadmap tracks the work required to run the portable, non-Hive, and
non-HBase portions of the legacy suites under `core/sql/regress` against the
standalone local-lite SQLCI and RocksDB store.

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

Extend row encoding, expressions, predicates, keys, and assignment for
UTF8/UCS2 and required translations/collations, then INTERVAL, BOOLEAN,
BINARY/VARBINARY, LONG VARCHAR, and the deliberately selected LOB boundary.

## Milestone 7: Advanced Executor Coverage

Add rowsets, cursors, positioned DML, window/OLAP execution, spill and scratch
management, executor statistics, cancellation cleanup, compound statements,
partition access, and a deliberate single-process or ESP boundary.

## Milestone 8: Authorization

Implement users, roles, ownership, GRANT/REVOKE, privilege metadata lifecycle,
definer/invoker rights, multi-identity SQLCI execution, and plan invalidation.

## Milestone 9: UDR

Implement library lifecycle, native and Java UDR loading, scalar/table
functions, procedures, result sets, transaction behavior, isolation, and error
recovery.

## Milestone 10: Suite Convergence

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
