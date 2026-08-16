# Local-Lite Legacy Regress Roadmap

## Purpose

Milestones 0 through 10 in this roadmap track the work required to run the
portable, non-Hive, and non-HBase portions of the legacy suites under
`core/sql/regress` against the standalone local-lite SQLCI and RocksDB store.
Milestones 11 through 13 extend that validated SQL surface into the session,
service, transaction, recovery, and unified-storage boundaries required for a
production-oriented single-node database. Milestone 14 makes TPC-C
qualification the next primary objective and uses those foundations to drive
isolation, concurrent execution, workload, and operational evidence.

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
UPSERT USING LOAD path remains intentionally unsupported. At M2 completion,
cross-table commit was not atomic; M12 later added durable multi-table DML
publication and restart recovery for the single-node compatibility layout.

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
`localLite/TEST041`, and the full local-lite lane passes 43/43. Object creation
reads the effective identity from the current `ContextCli`, not an environment
variable or a potentially stale compiler-session mirror.

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
`charsets/TEST003`, `TEST004`, `TEST010`, `TEST314`, `TEST316`,
`core/TEST018`, `core/TEST163`, `executor/TEST014`, `executor/TEST050`,
`executor/TEST101`, and `seabase/TEST012[schemaDrop,getStmts]`; the native lane
remains 43/43.
This was revalidated on 2026-08-15; all eleven allowlisted legacy cases and all
43 native cases passed with exact normalized EXPECTED output.
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
| M10A | bounded complete | CTAS and volatile CTAS use the ordinary local DDL path; logical PARTITION/DIVISION/STORE BY/table-attribute hints are accepted as non-physical hints; view mutability and WITH CHECK OPTION metadata are persisted and exposed to the binder | `executor/TEST014`, native `TEST009`; the 120-second `core/TEST029` re-probe now exits normally but remains blocked on reviewed SHOWDDL and view-DML diagnostic differences |
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

The remaining legacy inventory is explicit rather than silently filtered: 52
section rows are still blocked by unsupported SQL/metadata, legacy output
contracts, adapter gaps, or bounded timeouts; 50 are unsafe because they require
shell/service-stack behavior, and 21 are excluded for HBase/Hive/physical-
storage behavior. Deduplicated by suite/TEST, the 122 inputs are 11 runnable,
51 blocked, 42 unsafe, and 18 excluded. In particular,
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

The 2026-08-12 M2-M6 re-probe is recorded in
`localLiteLegacy/reprobe-m2-m6-2026-08-12.tsv`. It executed all 49 safe blocked
sections and reran every initial timeout with a 120-second per-entry limit. Five
exact baseline matches were promoted (`charsets/TEST004`, `TEST010`, `TEST314`,
`executor/TEST050`, and `seabase/TEST012[schemaDrop,getStmts]`); 29 retained
output differences, 13 timed out, and `charsets/TEST001` plus
`executor/TEST012` aborted in string result rendering.
Manifest blockers now describe these observations instead of the superseded
M2-M6 prerequisite labels. A timeout or output difference is not counted as a
failed standard-suite execution because these rows remain explicit probes.

The broader standard inventory is also explicit. `standard-extra-manifest.tsv`
classifies all 14 Hive cases as excluded and all 25 QAT cases as blocked pending
an adapter that preserves their ordered shared schema/data state. Together with
the 122 adapted inputs this gives the complete 161-case `runallsb` inventory.
The separate `newregr-inventory.tsv` currently accounts for 281 paired assets,
one unpaired MVS input, and the custom `perf`/`exeperf` workloads. These assets
do not contribute to the standard pass rate and have no execution result until
suite-specific state and baseline adapters exist.

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

The completion labels below are scoped. Completed M1-M10 milestones cover their
declared RocksDB-only compatibility surface; completed M11 covers its local
trusted multi-session service boundary. Neither label implies full legacy-suite
convergence or a production database.

| Milestone | Status | Current evidence and boundary |
| --- | --- | --- |
| M1 UPDATE | Complete | Native `TEST026`; UPDATE generation/execution, key changes, statement atomicity, explicit transaction behavior, and diagnostics are covered. |
| M2 DELETE, UPSERT, MERGE | Complete | Native `TEST027`-`TEST029`; M2 established per-table atomic publication, and M12 later added journal-coordinated multi-table DML publication/recovery. |
| M3 secondary indexes | Complete | Native `TEST030`-`TEST035` plus RocksDB SQLCI smoke; equality, prefix, range, covering/index-only access, uniqueness, and DML maintenance are covered. |
| M4 catalog DDL and constraints | Complete for the declared local surface | Native `TEST009`, `TEST021`, `TEST036`, `TEST037`; later metadata/DDL compatibility work and `TEST043` extend this surface. RI CASCADE and computed system columns remain outside the boundary. |
| M5 metadata and statistics | Complete for the declared local surface | Native `TEST038`; SHOWDDL/SHOWSTATS and persisted row/NULL statistics are covered. Full histograms and unrestricted legacy physical `_MD_` behavior are not claimed. |
| M6 character sets and data types | Complete for the declared local surface | Native `TEST039`; ISO88591/UTF8/UCS2, binary types, BOOLEAN, INTERVAL, and LONG VARCHAR are covered. LOB/ARRAY and broader collation/translation behavior remain outside the boundary. |
| M7 advanced executor | Complete for the single-process surface | Native `TEST040`; cursors, windows, grouping, sorting, cancellation cleanup, and local scratch lifecycle are covered. There is no ESP fan-out or remote multi-session runtime. |
| M8 authorization | Complete for the local catalog surface | Native `TEST041`; users, roles, ownership, privileges, revoke checks, and view owner/invoker boundaries are covered. Password authentication and an external identity service are not. |
| M9 UDR | Complete for the bounded adapter surface | Native `TEST042`; versioned routine metadata and bounded native/Java invocation are covered. This is not the full UDR server or host-rowset surface. |
| M10 suite convergence | Bounded complete; full portable legacy convergence remains incomplete | `make local-lite-m10` covers the eleven allowlisted legacy entries and native `TEST001`-`TEST043`. The remaining inventory is still classified as blocked, unsafe/service-stack-dependent, or excluded physical HBase/Hive behavior. |
| M11 sessionized runtime and standalone server | Complete for the declared local trusted surface | `make local-lite-m11` covers per-session transaction state, two-`ContextCli` SQLCI behavior, a multi-client server with clean/unclean restart, and a reduced Trafodion Type 4 endpoint through the repository T4 JDBC driver. M14E later removes the original compiler/executor queue with session-thread ownership and narrow DDL/utility locks. M12 supplies the bounded single-node recovery layer, while authentication/TLS and broader security remain later work. |
| M12 transactional storage and recovery | Complete for the declared single-node boundary | `make local-lite-m12` covers the common TransactionDB/SQLite contract, backend selection, versioned metadata-key migration, recovery/operations faults, and real SQLCI multi-table commit interruption/restart recovery. Node HA and distributed execution are not claimed. |
| M13 exclusive unified storage | Complete for single-process format activation | `make local-lite-m13` includes M12 and proves an after-format interruption, retry without cleanup, explicit rejection of old `catalog/` or `data/` layouts, unified-only DDL/DML/drop, and restart persistence. Old-layout migration/fallback is intentionally absent; zero-downtime orchestration, journal consolidation, and backup scheduling remain later work. |
| M14 TPC-C qualification | Complete for repeatable TPC-C-like single-node qualification | The pinned contract, loader, five profiles, Level 3 matrix, six durable-decision crash cases, concurrent runtime, and two-warehouse operations gate pass. `make local-lite-m14` explicitly includes M10-M13 and M14A-M14F, composes one report, and separates functional/TPC-C-like passes from formal-compliance failure. Client-side writer admission and the mix/pacing remain non-compliant; no `tpmC` claim is made. |
| M15 Trafodion MVCC/OCC | Complete for the declared single-node correctness and repeatable engineering baseline; production SLO incomplete | Seven focused commits replace database-wide validation and client admission with transaction snapshots, key/range read sets, post-start OCC validation, transactional index reads, and atomic delta publication. `make local-lite-m15g` passes Release qualification at 32 warehouses/32 offset terminals with all five profiles, zero conflicts/retries/unclassified errors, and operations recovery. The 2026-08-16 result is 1.130 TPS and 135.075 s Stock-Level p95, below the separate 50 TPS and 2 s production targets; no formal TPC-C or `tpmC` claim is made. |

## Milestone 11: Sessionized Runtime And Standalone Server

Status: complete for the declared local trusted surface. M11 moves local-lite
from a single SQLCI process into one long-running database service that owns the
embedded store and safely hosts multiple client sessions. It deliberately does
not select or replace the storage engine, claim production recovery, or restore
the Trafodion service stack.

### M11A: Session-Owned Transaction Context

- Replace the process-global `LocalLiteTxnState` singleton with a transaction
  context owned by each `ContextCli` session.
- Keep `LocalLiteStorageManager` process-owned so one server process owns and
  shares the catalog/table handles.
- Resolve the current transaction context from executor statement/session
  state instead of implicit static access.
- Release statement snapshots on cancellation, and roll back an active
  transaction when a session disconnects, resets, or is destroyed.
- Preserve the current `LocalLiteTxn` executor-facing API boundary so scan and
  DML TCB semantics do not depend on a future wire protocol.

Current implementation evidence (2026-08-13): each `ContextCli` owns an
`ExTransaction`, which creates and canonically owns that session's
`LocalLiteTxnContext`. Transaction, tuple-flow, scan/DML, DDL, and executor-root
statement-snapshot paths receive that context explicitly; reset, end/drop
session, deletion, and destruction discard its pending writes and snapshots.
Making `ExTransaction::xnInProgress()` authoritative also exposed inherited
native-HBase user-transaction DDL restrictions; LocalLite keeps its existing
bounded CREATE/DROP and CTAS behavior explicitly, revalidated by
`executor/TEST014`. Object ownership is likewise bound to the current
`ContextCli`, with `TEST041` covering identity switches and owner GRANTs.
`make local-lite-m11a` exercises the static wiring plus statement, transaction,
concurrency, lower-level independent-context, and production two-`ContextCli`
SQLCI probes. The SQLCI probe keeps transactions active in both contexts,
validates isolation and independent completion, then uses `RESETCONTEXT` and
`DELETECONTEXT` to verify implicit rollback without poisoning the peer.
The complete M11 gate adds a real cancel request while a peer transaction is
active: the canceled statement returns `57014`, its snapshot is released, the
transaction rolls back explicitly, and the peer commits and remains usable.

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

Implemented evidence: `nativelite-server` opens a process-lifetime catalog
lease, rejects a second server on the same store, creates/destroys one
`ContextCli` per connection, and accepts numeric loopback TCP or an exact Unix
socket with mode `0600`. At M11, connection threads fed one serialized engine
queue; M14E later replaces it with session-thread execution and narrow
DDL/utility locks. Completed
connection threads are reaped. Unix startup refuses non-socket, foreign, or
active paths, and shutdown unlinks only the exact socket inode created by this
server. The `make local-lite-m11b` lifecycle gate verifies store ownership,
disconnect rollback and key reuse, then validates committed versus pending data after both
graceful shutdown and `SIGKILL` restart. Health and Unix-socket paths are also
covered, including preservation of an existing regular file and a pathname
replacement made after bind. Session diagnostic captures are unnamed and the
unclean-restart path verifies that no SQL-bearing capture pathname is exposed.

### M11C: Product Client Protocol

- Use a reduced Trafodion Type 4 client path without restoring DCS, ZooKeeper,
  Monitor, or the full service stack.
- Support authentication startup, statement lifecycle, result metadata/rows,
  transaction status, cancellation, and stable SQLSTATE mapping.
- Validate with a real JDBC or ODBC client.  An in-process CLI call is not
  end-to-end protocol evidence.

M11C selects a reduced Trafodion Type 4 endpoint. One loopback listener handles
both the association handshake and SQL dialogue, so DCS and ZooKeeper are not
required. The implemented calls are connect/disconnect, set connection option,
end transaction, prepare, execute/direct execute, fetch, free statement,
STOPSRVR cancellation, and bounded GetSQLCatalogs support for catalogs, schemas,
tables, columns, and primary keys. `make local-lite-m11c` compiles the repository
T4 driver and runs `NativeLiteT4JdbcTest`; it covers prepared reuse, typed rows,
overlapping transactions, disconnect rollback, cancellation with an active
peer, metadata, and restart persistence. The driver's public
`Statement.cancel()` does not dispatch during an active request, so the gate
uses its internal `T4_Dcs_Cancel` path.

M11 completion does not claim production readiness. The endpoint is trusted
local only: TCP is loopback-only, Unix sockets are owner-only, and there is no
password exchange. Results are buffered. M11 established the multi-session
service boundary later made compiler/executor-concurrent by M14E and consumed by M12
durability, recovery, backup, and resource-governance work. Password/TLS
authentication and broader security hardening remain later milestones.

## Milestone 12: Transactional Storage And Recovery

Status: complete for the declared single-node boundary. M12 makes the
M11 service crash-recoverable for durable multi-table DML and establishes the
transactional target format and operations contract. Storage-engine selection
was made by the common gate rather than baked into the executor.

The requested implementation order is complete:

1. Rebuild and pass the M10 and M11 input baselines before changing storage.
2. Introduce the backend-neutral engine/session/transaction/status contract.
3. Add a bounded, cancellable streaming range cursor.
4. Define one ordered transaction key space for catalog, base, UNIQUE, and
   index records, plus durable compatibility-layout SQL publication/recovery.
5. Migrate fixed-buffer metadata keys to collision-free version-2 encodings.
6. Run TransactionDB and SQLite WAL through the identical gate and select
   TransactionDB from correctness, recovery, and operational evidence.
7. Complete recovery, checkpoint, backup/restore, integrity, fault injection,
   metrics, disk-watermark, and SQL restart gates.

### M12A: Storage Contract And Atomic Transaction Domain

- Introduce backend-neutral `StorageEngine`, `StorageSession`, `StorageTxn`,
  and streaming `StorageCursor` contracts.
- Put catalog metadata, base rows, primary/UNIQUE records, and secondary-index
  maintenance inside one transaction domain.
- Replace whole-table materialization at the storage API with bounded streaming
  range cursors.
- Version and migrate the pre-M11 persisted metadata-key encoding before
  changing its fixed buffer layout, and add collision/integrity checks to the
  migration gate.
- Define isolation, conflict, retryable error, cancellation, and synchronous
  durability contracts before selecting the backend.
- Preserve versioned row/key/catalog formats or provide an explicit migration.

Implemented evidence (2026-08-14): `LocalLiteStorage.h` defines the engine,
session, transaction, and one-record-at-a-time bounded cursor contracts plus
stable status/metric types. The common contract places catalog, base-row,
UNIQUE, and index records in one transaction and verifies commit, rollback,
snapshot isolation, deterministic retryable conflicts, cancellation, streaming
range iteration, and synchronous durability. Existing metadata COLUMNS and
KEYS records migrate at open to collision-free version-2 keys containing full
UID/ordinal/key identifiers; downgrade simulation verifies exact regenerated
counts and absence of legacy keys.

### M12B: Backend Selection And Implementation

- Evaluate at least the single-database RocksDB TransactionDB layout and one
  transactional embedded alternative against the same correctness, recovery,
  and workload tests.
- Select the backend using transaction correctness and crash recovery first,
  p95/p99 stability and operational behavior second, and peak throughput after
  those gates.
- Do not map a successful point-get/scan prototype to production readiness.

Implemented evidence: the same executable contract and crash/fault workload
runs both RocksDB TransactionDB and SQLite WAL. Both pass correctness and
recovery; the gate records p50/p95/p99 and transaction rate without selecting
on peak throughput. RocksDB TransactionDB is selected because it satisfies the
correctness/recovery gates while preserving RocksDB operational and migration
continuity for supported metadata-key formats.

### M12C: Recovery, Backup, And Operations

- Add synchronous commit/WAL policy, checkpoint, integrity verification,
  consistent backup, restore, and on-disk format-version checks.
- Inject process kill, restart, disk-full, interrupted checkpoint/backup,
  commit-boundary, and metadata/data publication failures.
- Add storage and transaction metrics, disk-watermark protection, and a
  repeatable restore drill.

Implemented evidence: the selected engine uses synchronous commit/WAL flush,
supports checkpoints, consistent backup, restore into a fresh path, full-key
space verification, transaction/byte/key metrics, and a configurable minimum
free-space watermark. The fault matrix covers committed and uncommitted process
exit, before/after commit, interrupted checkpoint/backup, restore, and
`UINT64_MAX` disk-watermark rejection. The SQL recovery gate stops after the
first of two table batches and proves the next process replays exactly once and
exposes both committed rows.

M12's selected single-keyspace contract provides the catalog/data/index atomic
domain and cross-keyspace snapshot. During the live-layout transition, SQL DML
uses a durable commit journal: every touched table is conflict-checked before
the commit decision, publication is process-serialized, and each table batch
contains an idempotent journal marker. Startup completes committed interrupted
publication before accepting SQL traffic; aborted/conflicting work never gets
a journal. M12 still does not claim node-level high availability,
multi-process writers, or distributed execution.

## Milestone 13: Exclusive Unified Storage

Status: complete for single-process format activation. Fresh stores are created
directly in `transactiondb/`. Startup rejects unknown or inconsistent format
and layout markers and commits `m13/active=1` with
`m13/layout=unified-hex-v1` before routing SQL catalog or table access.

`TRAF_LOCAL_LITE_ACTIVATION_FAULT=after-format` terminates before activation;
retry finishes activation without cleanup. Startup rejects any store containing
old `catalog/` or `data/` directories before creating a target.
`make local-lite-m13` proves that rejection, activation retry, active reads,
updates, index/table cleanup, new DDL/DML, and restart persistence. The old
per-table migration, export, rollback, and runtime fallback code is removed.

This is an offline operational switch, not a zero-downtime rolling upgrade.
The M12 TransactionDB recovery journal remains a separate store while SQL
multi-table publication uses the existing decision/marker/replay protocol.
Service drain/upgrade orchestration, active-layout backup policy, journal
consolidation, node HA, and distributed execution remain outside M13.

## Milestone 14: TPC-C Qualification

Status: complete for repeatable TPC-C-like single-node qualification. M14 turns the completed
M11-M13 session, transaction, recovery, and unified-storage foundations into a
repeatable OLTP qualification workload. It is not complete when the schema can
be loaded or when one transaction succeeds: completion requires all five TPC-C
transaction profiles, concurrent correctness, crash recovery, and explicitly
versioned evidence.

The normative benchmark reference is
<https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf>.
Repository tests may use a dialect-adapted schema and a non-audited driver, but
every deviation from the specification must be recorded. Until every formal
requirement is satisfied, results must be labelled `TPC-C-like` or
`non-compliant`; they must not be reported as official `tpmC`.

The requested implementation order is:

1. Freeze the specification, schema mapping, workload contract, and baseline.
2. Build deterministic schema creation, loading, and consistency checks.
3. Implement the five transaction profiles through the real client boundary.
4. Close ACID, Level 3 isolation, and conflict/retry correctness gaps.
5. Remove the compiler/executor serialization bottleneck safely.
6. Add multi-warehouse scale, latency, throughput, and recovery evidence.
7. Compose the qualification gate and publish an explicit compliance report.

### M14A: Specification, Inventory, And Reproducible Baseline

Status: complete. `benchmarks/tpcc/qualification.properties` pins TPC-C 5.11.0,
the one-warehouse scale, deterministic seeds, batch/commit policy, initial
terminal count, retry policy, and report version. `schema-manifest.tsv` maps all
nine entities and `dialect-deviations.tsv` makes the non-compliant boundary
explicit. `make local-lite-m14a` validates those assets and emits the expected
cardinalities as JSON.

- Pin the TPC-C specification revision and record all SQL/type/name mappings.
- Add a versioned manifest for the nine logical entities: WAREHOUSE, DISTRICT,
  CUSTOMER, HISTORY, NEW_ORDER, ORDER, ORDER_LINE, ITEM, and STOCK. Reserved
  identifiers may be mapped, but the mapping must remain stable and documented.
- Define one warehouse as the first correctness scale. Record deterministic
  random seeds, cardinalities, key ranges, data distributions, and loader
  parameters so two clean runs produce equivalent logical databases.
- Choose and vendor or implement a driver whose license, source revision,
  transaction mix, terminal model, pacing, and retry policy are inspectable.
- Capture the existing `make local-lite-m10`, `make local-lite-m11`, and
  `make local-lite-m13` results before changing concurrency or isolation.

Gate: `make local-lite-m14a` validates the pinned inputs and produces a
machine-readable baseline report with the exact pre-generation cardinalities.
M14B consumes that contract to create an isolated store, deterministically
generate ORDER_LINE cardinality, load one warehouse, and verify database
invariants. This separation keeps contract drift distinguishable from loader or
database failures.

### M14B: Schema, Loader, And Data Integrity

Status: complete. `benchmarks/tpcc/schema.sql` creates all nine mapped tables,
core foreign keys, and customer/order lookup indexes. `NativeLiteTpcc` creates
and deterministically loads the database through the reduced T4 endpoint using
bounded 1,000-row set commits; an injected failure proves restartable loading.
The one-warehouse result contains 100,000 ITEM, 100,000 STOCK, 30,000 CUSTOMER,
30,000 HISTORY, 30,000 ORDER, 9,000 NEW_ORDER, and exactly 300,003 ORDER_LINE
rows. Exact counts and full relationship anti-joins pass on the initial store,
after a clean restart, and against a copied TransactionDB store. The
high-cardinality ITEM/STOCK and ORDER/ORDER_LINE relationships remain an
explicit dialect deviation: enforcing them row-by-row as declared foreign keys
is not currently viable at qualification scale, so the gate verifies them with
full anti-joins.

- Support the exact integer, fixed numeric, character, timestamp, default,
  primary-key, UNIQUE, secondary-index, and bounded referential-integrity
  shapes required by the mapped schema.
- Keep loader commits bounded and restartable. A failed batch must not leave a
  partially accepted logical unit or silently skip rejected rows.
- Add post-load consistency queries for district next-order identifiers,
  customer/history relationships, orders/order-lines, new-order queues, and
  item/stock coverage.
- Validate clean restart and TransactionDB backup/restore against the loaded
  database before transaction traffic begins.

Gate: a fresh load, interrupted load/retry, restart, and restore all produce the
same declared counts and consistency-query results.

### M14C: Five Transaction Profiles Through T4 JDBC

Status: complete for deterministic functional profiles. The repository driver
implements New-Order, Payment, Order-Status, Delivery, and Stock-Level as
explicit reusable prepared-statement programs. `make local-lite-m14c` loads a
fresh smoke store, runs every profile in isolation, injects rollback and
duplicate-key failure, proves disconnect rollback, then starts two concurrent
T4 terminals. The gate commits three of each profile, classifies optimistic
`restart transaction` conflicts for at most three retries with backoff, reports
zero unclassified errors, verifies exact row deltas and orphan-free references,
and repeats the effect checks after server restart. The T4 server now preserves
prepared statements across ResultSet `SQL_CLOSE`, drops them only for
`SQL_DROP`, and emits a correctly shaped EndTransaction error descriptor.
Normative random mix, customer last-name selection, variable order-line count,
Delivery across all ten districts, pacing, and scale remain explicit M14F work.

- Implement New-Order, Payment, Order-Status, Delivery, and Stock-Level as
  explicit prepared-statement transaction programs.
- Exercise commit, rollback, typed parameter binding, typed fetch, NULL and
  timestamp handling, duplicate/conflict diagnostics, disconnect cleanup, and
  statement reuse through `jdbc:t4jdbc://127.0.0.1:23400/:`.
- Keep application loops and branch decisions visible in the driver; do not
  replace a required transaction with a server-side shortcut solely to obtain
  a passing workload.
- Assert each transaction's row-level effects and the cross-table consistency
  conditions after both success and injected failure.

Gate: `make local-lite-m14c` runs each profile deterministically in isolation,
then runs the five-profile mix with at least two concurrent terminals and zero
unclassified SQL errors or consistency violations.

### M14D: Isolation, Conflicts, And Durability

Status: complete for the declared single-node optimistic boundary.
`make local-lite-m14d` runs a real two-session T4 JDBC matrix covering dirty
read, dirty write, non-repeatable read, predicate phantom, write skew, and one
bounded retry with exactly-once effects. Transactions retain stable snapshots
and capture the unified TransactionDB sequence at begin. Commit first performs
exact row-before-image validation, preserving precise same-key diagnostics,
then rejects a writing transaction if any database sequence changed since its
snapshot. This conservative database-wide validation closes predicate and
write-skew gaps, but can abort independent writers; D009 records that disclosed
tradeoff.

The policy is non-blocking optimistic abort with a retryable `restart
transaction` diagnostic. The gate requires conflict response within five
seconds; because transactions do not wait for locks, deadlock cycles are
avoided rather than detected. The M14C client permits three retries with
backoff, while the isolation proof uses one explicit retry and verifies only
one logical effect. The M14D crash matrix terminates the server before and
after the synchronous journal decision for New-Order, Payment, and Delivery.
All six stores restart with either none or all of each profile's cross-table
effects and pass relationship checks.

- Implement and prove the Level 3 boundary required among New-Order, Payment,
  Delivery, and Order-Status: no dirty write, dirty read, non-repeatable read,
  or phantom within the required transaction pairs.
- Add predicate/range conflict tracking or another disclosed mechanism that
  prevents write skew and phantoms; snapshot isolation alone is not accepted as
  evidence of serializable behavior.
- Define blocking versus optimistic-abort behavior, conflict diagnostics,
  deadlock detection or avoidance, lock/conflict timeout, and bounded client
  retry with backoff. Retries must preserve exactly-once logical effects.
- Encode every isolation test required by the pinned TPC-C revision, adapting
  the procedure only where the non-locking design requires an equivalent proof.
- Inject process termination before and after the durable commit decision for
  New-Order, Payment, and Delivery, then verify atomicity and consistency after
  restart.

Gate: `make local-lite-m14d` passes the versioned isolation matrix, consistency
conditions, commit/rollback faults, and crash-recovery cases without relying on
global single-statement serialization.

### M14E: Concurrent Compiler And Executor Runtime

Status: complete for the bounded connection-thread runtime. The versioned
`m14e-runtime-inventory.tsv` records the compiler, CLI, diagnostics, executor,
scratch, authorization, schema, store, and cancellation ownership decisions.
Non-stop requests no longer enter the initialization worker queue. Each
connection thread selects its session ContextCli and uses thread-local CLI
current-context, SQLCI-environment, assertion-target, and default-schema state.
Compiler, executor, transaction, diagnostic capture, and statement state remain
owned by the session ContextCli. CREATE/DROP/ALTER/INITIALIZE and SQLCI
compatibility utilities use separate narrow mutexes; storage publication keeps
its existing atomicity latch.

`make local-lite-m14e` loads a smoke database, executes five repeated pairs of
real COUNT queries with deterministic test-only overlap holds, and requires the
server's compiler/executor-region high-water mark to be at least two. It also
creates the same unqualified table name in two session schemas, checks isolated
results and diagnostics, and reruns the T4 cancellation, disconnect rollback,
peer-survival, and restart suite. The concurrency regression exposed previously
uninitialized ContextCli UDR-policy fields as repeatable SQLCODE `-8884`; every
new context now initializes those fields explicitly. The debug toolchain has no
maintained ThreadSanitizer target, so five deterministic races plus the M14C,
M14D, and T4 lifecycle gates are the declared equivalent instrumentation limit.

- Inventory all process-global compiler, CLI, diagnostics, executor, scratch,
  authorization, and current-context state currently protected by the server's
  global request serialization.
- Move mutable request state to session/request ownership or introduce narrow,
  documented synchronization around genuinely shared caches and catalogs.
- Permit independent sessions to compile and execute concurrently while
  preserving DDL/catalog serialization where required.
- Add race, cancellation, disconnect, slow-client, and peer-survival gates;
  run sanitizer or equivalent concurrency instrumentation on the new ownership
  boundary where the toolchain permits it.

Gate: overlapping terminals must show measured executor overlap, preserve
transaction isolation, and complete without cross-session diagnostics, state
leaks, deadlocks, or store corruption. Merely accepting concurrent sockets does
not satisfy M14E.

### M14F: Scale, Performance, And Operations

Status: complete for the declared non-compliant operations scale.
`make local-lite-m14f` loads two warehouses with two districts and 100
customers/orders each, then runs two terminal sessions for 5 warmup and 20
measured transactions per terminal across two repetitions. The fixed
45/40/5/5/5 mix reports committed/aborted/retried counts, p50/p95/p99/max
latencies, total throughput, and a versioned 0.75 maximum variance ratio.

The reduced executor retains the concurrent path measured by M14E. The M14F
workload uses fair client-side admission for whole writer transactions to avoid
false conflicts from M14D's conservative database-wide sequence validator.
D008 and D010 record the non-random mix,
missing normative pacing, and serialized writer boundary. This remains useful
single-node operations evidence but is not a compliant terminal model or
`tpmC` measurement.

The gate creates a consistent RocksDB TransactionDB checkpoint while the
workload is live, verifies relationships on the live store, after clean and
SIGKILL restart, and from the checkpoint store, and accepts the documented
disk-watermark rejection at startup or write time. Its report includes
source/runtime identity, RSS, store-size delta, and recovery timing. Metrics not
exposed by the reduced T4/RocksDB C API are explicitly marked unavailable.

- Scale from one warehouse to multiple warehouses without changing the schema,
  transaction programs, or correctness assertions.
- Generate the declared TPC-C transaction mix and terminal behavior; report
  committed/aborted/retried transactions separately by profile.
- Record throughput plus p50/p95/p99/max latency, queue time, compile time,
  execution time, conflict rate, retry count, WAL/fsync latency, compaction,
  write stalls, cache behavior, disk growth, and recovery time.
- Establish warmup, measurement duration, repetitions, variance threshold,
  hardware/software identity, and artifact retention before setting a numeric
  performance target.
- Exercise checkpoint/backup under load, clean restart, unclean restart,
  disk-watermark rejection, and restored-database consistency.

Gate: `make local-lite-m14f` produces a reproducible non-compliant performance
report and passes correctness/recovery at every declared scale. Performance
regressions fail only against versioned thresholds, never an ad hoc best run.

### M14G: Aggregate Qualification And Reporting

Status: complete. `make local-lite-m14` has explicit M10, M11, M12, M13, and
M14F prerequisites; the M14F dependency retains the whole M14A-M14E chain.
`m14-regression-inputs.tsv` prevents those inputs from becoming implicit.
`test-local-lite-tpcc-qualification.sh` validates the three claim checklists and
composes the actual M14F workload/operations JSON with schema, driver, config,
source, isolation, recovery, and deviation evidence into
`qualification-report.json` under `M14_REPORT_DIR`.

The aggregate gate also exercises the pre-M14 SQL surface against M14D's
database-wide serializable validator.  It caught and now covers two
transaction-owned unified-store writes that must advance the current
transaction baseline rather than look like external conflicts: CTAS catalog
creation before its row commit and identity/sequence range allocation before
its INSERT commit.

- Add `make local-lite-m14` as the aggregate M14A-M14F gate and keep M10-M13
  regression inputs explicit in CI so TPC-C work cannot narrow prior coverage.
- Publish schema/driver revisions, configuration, scale, transaction counts,
  errors, retries, latency distributions, resource metrics, consistency and
  isolation results, recovery evidence, and known deviations in one report.
- Separate three claims: transaction-profile functional support, repeatable
  TPC-C-like workload support, and formal TPC-C compliance. Each has its own
  checklist and no stronger label is inferred from a weaker gate.

M14 completion means repeatable TPC-C-like qualification on the supported
single-node trusted-local boundary. Formal benchmark compliance additionally
requires all specification, timing, pricing, audit, and disclosure obligations
outside the code-only gate. Password/TLS, node HA, distributed execution, and
official publication remain separate productization work unless explicitly
added to the tested system boundary.

## Milestone 15: Trafodion MVCC/OCC

Status: complete for the declared single-node correctness and repeatable
engineering baseline. Production performance targets remain incomplete. M15
keeps TransactionDB for snapshot and atomic persistence, does not introduce
SSCC, and removes M14's database-wide sequence validator and client writer
admission.

### M15A-M15F: OCC Runtime

- M15A pins the Trafodion MVCC/OCC contract and machine-readable metrics.
- M15B retains one unified snapshot for the whole transaction.
- M15C records point keys, scan/predicate ranges, and write keys.
- M15D validates overlap only against writes committed after the transaction
  start sequence, including write skew and phantom cases.
- M15E makes secondary-index point/range/covering reads participate in the
  same snapshot and conflict model.
- M15F publishes atomic per-key deltas through TransactionDB and retains the
  durable multi-table recovery decision.

### Milestone 16: Stock-Level Range Aggregation and Index Optimization

Status: implementation complete; Release runtime evidence is pending an
environment that permits the NativeLite TCP listener. M16 addresses the M15
performance bottleneck without changing its Trafodion MVCC/OCC transaction
model.

- M16A fixes `stock-level-contract.tsv`, optimization metrics, phase commits,
  and the zero-full-scan policy.
- M16B adds `TPCC_ORDER_LINE_STOCK_IX` with the range-leading key order and
  validates its catalog and physical metadata.
- M16C changes Stock-Level to an index-backed order-line range scan, distinct
  `(OL_SUPPLY_W_ID, OL_I_ID)` aggregation, and stock primary-key point reads
  under one transaction snapshot.
- M16D proves the optimizer/generator emits the ordered range access path and
  that the new result equals the original join/count-distinct result.
- M16E adds the M16 plan and correctness gate to the build surface.
- M16F runs the Release workload, records scan/latency deltas, and enforces
  zero Stock-Level full scans while retaining production targets as targets.
  The current host reaches the Release build but the runtime gate is blocked
  by `NativeLite server startup failed: create TCP socket: Operation not
  permitted`; no substitute performance evidence is accepted.
- M16G synchronizes all roadmap and benchmark documentation and records the
  final evidence. Formal TPC-C and `tpmC` claims remain excluded.

### M15G: Release OCC Qualification

`make local-lite-m15g` reruns M15A-M15F, builds Release, loads an explicitly
reduced TPC-C-like engineering population, runs 32 warehouses and 32 terminal
schedules offset across the 45/40/5/5/5 mix, and verifies online checkpoint,
clean/unclean restart, checkpoint restore, disk watermark, consistency, and
claim boundaries. The shared per-repetition deadline prevents timeout from
being multiplied by the terminal count. Loader tasks use independent T4
sessions for disjoint warehouse partitions and refresh the coordinator's MVCC
snapshot at phase boundaries.

The verified 2026-08-16 report records 1.130 TPS, 3.423% three-run variance,
zero conflicts, retries, and unclassified errors, and five non-zero profile
samples. P95 latency is 3.301 s New-Order, 1.312 s Payment, 0.876 s
Order-Status, 1.672 s Delivery, and 135.075 s Stock-Level; recovery is
161-214 ms. Regression thresholds preserve this baseline, while the config
separately records production targets of 50 TPS and 1/0.5/0.5/2/2 seconds.
M15 completion therefore establishes OCC correctness and repeatability, not
production SLO certification, formal TPC-C compliance, or `tpmC`.
