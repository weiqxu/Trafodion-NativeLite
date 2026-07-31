# Trafodion Local-Lite

## Overview

`local-lite` is a native-only Trafodion build and runtime mode for local SQL
engine development. It removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS,
REST, TrafCI, and Java client modules from the selected build path.

The current useful runtime target is `sqlci` in local-lite mode. It can start as
a single local process, execute SQL that stays inside the compiler/executor
such as `SELECT` statements over `VALUES` clauses, and run a narrow
RocksDB-backed local table smoke path.

Local-lite is not a complete standalone database yet. It now includes a minimal
RocksDB-backed local table path for `sqlci` table smoke tests, but the full
Trafodion catalog, complete table integration, transactions, indexes,
privileges, JDBC/ODBC connectivity, and distributed execution remain outside
the supported runtime surface. The current local NATable loader and executor
scan TCB are intentionally narrow and only cover the v1 local table smoke path.
The transaction and concurrency integration plan is tracked separately in
[`local-lite-transaction-roadmap.md`](local-lite-transaction-roadmap.md).

## Current State

The implementation is compile-time gated by `TRAF_LOCAL_LITE=1` and
`-DTRAF_LOCAL_LITE`. The default Trafodion build remains available through
normal targets such as `make all`.

Implemented:

- Top-level `make local-lite` target.
- Native SQF/SQL build graph with Java/Maven/HBase/Hadoop modules trimmed.
- Local-lite defaults in `core/Makefile` for `TRAF_HOME`, `SQ_MTYPE`,
  `SQ_MBTYPE`, MPICH, Thrift, log4cxx, and related native paths.
- TM local-lite mode that skips HBase transaction jar setup, `hbasetmlib2`,
  `libjvm`, and Java include paths.
- SQL build rules that skip Maven jar targets and avoid global JVM/HDFS link
  flags in local-lite mode.
- Executor, expression, optimizer, and TM stubs/guards for HDFS, Hive, HBase,
  Java/JNI, ORC, LOB, native expression, and fast transport paths.
- `sqlci` local-lite runtime initialization that skips monitor/MPI startup,
  sets local defaults, and creates required local directories.
- `sqlci` REPL startup and trivial query execution without starting monitor,
  TM/DTM/RMS, DCS, REST, ZooKeeper, HBase, or Hadoop.
- Script guards in `sqenvcom.sh`, `sqgen`, and `sqstart` when
  `TRAF_LOCAL_LITE=1`, so those legacy scripts skip Hadoop/HBase/ZooKeeper
  setup checks if they are used.
- RocksDB development dependency detection for local-lite SQL builds. Missing
  headers produce an explicit build error that names `librocksdb-dev`,
  `rocksdb-devel`, and `ROCKSDB_INC_DIR`.
- Minimal `sqlci` local table gate compiled into `libsqlcilib.so` only when
  `TRAF_LOCAL_LITE=1`. It reports known unsupported statements before prepare;
  supported local table queries are not executed by SQLCI.
- Minimal RocksDB catalog and table data store under
  `TRAF_LOCAL_STORE_DIR` or `TRAF_VAR/localstore/rocksdb`.
- Basic local table SQL in `sqlci`: compiler-routed `CREATE TABLE`,
  `DROP TABLE`, and executor-routed single-row and multi-row
  `INSERT INTO ... VALUES (...)`.
- Local catalog NATable loading for local-lite RocksDB tables.
- Local RocksDB executor scan TCB for compiler-generated local table `SELECT`
  plans. It scans RocksDB rows, maps the compiler projection through the
  fetched-column list, writes projected values into the compiler-generated
  binary aligned executor row, and returns rows through normal executor queues.
- Local-lite binding disables the HBase coprocessor `COUNT(*)` rewrite, so
  scalar aggregates run through the normal executor aggregate over a local
  executor scan.
- Local RocksDB executor insert TCB for compiler-generated local table `INSERT`
  plans. It evaluates the compiler-generated insert expression, normalizes the
  executor row into the local canonical binary aligned row layout, and persists
  the `LLBR1` payload through RocksDB.
- Local table inserts now go through an autocommit `LocalLiteTxn` facade, which
  combines row-id allocation and row persistence into one local storage manager
  operation instead of issuing separate calls from the executor TCB. The legacy
  public store APIs for direct row-id allocation and row put have been removed.
- Local table scans and get-row reads now go through the `LocalLiteTxn` facade
  with a statement execution token. All scan TCBs that access the same table in
  one executor statement reuse one RocksDB snapshot, which is released by the
  executor root at completion, cancellation, fatal error, or teardown.
- Local-lite `BEGIN WORK`, `COMMIT WORK`, and `ROLLBACK WORK` now use an
  initial local transaction context in single-process SQLCI mode. Transactional
  local INSERTs are buffered until commit, rollback discards them, and scans
  read their own pending writes over a per-table RocksDB snapshot retained
  across all statements in the transaction.
- Local RocksDB storage now shares catalog/table handles inside the local-lite
  process module, so multiple executor scans of the same table in one statement
  no longer reopen the same RocksDB path and collide on RocksDB `LOCK`.
- Local executor table rows preserve nullable values. Scan predicates and
  projections over NULL fixed-width and variable-width columns use the normal
  executor tuple descriptors.
- Local-lite scalar type coverage includes integer, floating-point, character,
  date/time/timestamp, exact `NUMERIC(p,s)`, `DECIMAL(p,s)`, and BigNum
  `NUMERIC(p,s)` values.
- Unsupported local table SQL diagnostics for `UPDATE`, `DELETE`, `MERGE`,
  `UPSERT`, and `CREATE INDEX`.
- HBase access TDBs reached through the old executor path now build a TCB that
  emits an unsupported diagnostic instead of returning `NULL`.
- Local-lite `make local-lite` no longer builds `make_monitor` or top-level SQF
  `tools`, because the supported runtime is single-process `sqlci`, not the SQF
  service stack.

Known limits:

- `sqlci` is the only runtime target intended to work standalone today.
- Other built binaries such as `tdm_arkcmp`, `shell`, `monitor`, `trafns`,
  `sqwatchdog`, `pstartd`, and `tm` may be present in the local-lite build, but
  they are not a supported standalone database environment.
- Local-lite `sqlci` still links internal Trafodion shared libraries and a small
  Seabed baseline. Monitor-specific paths are skipped or guarded, but Seabed
  libraries remain transitive dependencies.
- The current RocksDB DDL path uses the compiler DDL entry point for
  `CREATE TABLE` and `DROP TABLE`, and local tables can now be loaded as
  NATables from the local catalog. Local table inserts also fall through CLI
  prepare and execute through the local executor insert TCB. Type coverage is
  still intentionally narrow, but exact decimal/numeric values are now stored
  by preserving the compiler-generated executor physical bytes.
- `SELECT` over local RocksDB tables uses the local executor scan TCB. The scan
  TCB projects from persisted binary aligned rows into the compiler-generated
  executor row layout.
- Some disabled storage paths still exist as compatibility stubs. They should
  fail with unsupported-operation diagnostics when reached; they do not provide
  HDFS/Hive/HBase functionality.
- In the current repository build layout, set `TRAF_HOME` explicitly when
  running `sqlci` directly from `core/sql/lib/linux/64bit/debug`. The runtime
  self-location code assumes an installed `export/bin64d`-style layout, and
  `/proc/self/exe` resolves symlinks to the SQL output directory.

## Build

Install native dependencies:

```bash
scripts/install-local-lite-deps.sh --dry-run
scripts/install-local-lite-deps.sh -y
```

The installer covers C/C++ build tools, MPICH, Thrift, log4cxx, protobuf,
RocksDB, SQLite, curl, OpenSSL, readline, ncurses, bison, flex, Perl, Python,
and native development headers. It intentionally does not install Java, Maven,
Hadoop, HBase, or Hive.

On systems where MPICH headers are not laid out like Trafodion's legacy tools
tree, the installer creates a repository-local compatibility directory:

```text
core/sqf/opt/local-lite-mpich
```

Build the local-lite graph from the repository root:

```bash
OMPI_CXX=/usr/bin/g++ make local-lite
```

`OMPI_CXX=/usr/bin/g++` forces the OpenMPI C++ wrapper to use the system C++
compiler, which avoids failures when `PATH` contains another toolchain whose
startup objects do not match the host C runtime.

For a narrower SQL debug rebuild:

```bash
TRAF_HOME=$(pwd)/core/sqf make -C core/sql/nskgmake TRAF_LOCAL_LITE=1 SQ_MTYPE=64 SQ_BTYPE=d SQ_MBTYPE=64d linuxdebug
```

## Build Outputs

Main exported binaries are written or symlinked under:

```text
core/sqf/export/bin64d/
```

Typical entries include:

```text
monitor
trafns
sqwatchdog
pstartd
monmemlog
tm
trafconf
shell
sqlci
tdm_arkcmp
```

Exported shared libraries are under:

```text
core/sqf/export/lib64d/
```

SQL debug binaries and libraries are under:

```text
core/sql/lib/linux/64bit/debug/
```

## Running SQLCI Standalone

For the current repository build layout, set `TRAF_HOME` and `LD_LIBRARY_PATH`
explicitly:

```bash
export TRAF_HOME=$(pwd)/core/sqf
export TRAF_LOCAL_LITE=1
export TRAF_LOCAL_STORE_DIR=${TRAF_LOCAL_STORE_DIR:-/tmp/traf-local-lite-store}

SQL_LIBS=$(pwd)/core/sql/lib/linux/64bit/debug
SQF_LIBS=$(pwd)/core/sqf/export/lib64d
export LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS:${LD_LIBRARY_PATH:-}

$SQL_LIBS/sqlci
```

Example:

```sql
>>SELECT 1 FROM (VALUES(1)) AS t(x);

(EXPR)
------

     1

--- 1 row(s) selected.
>>exit;
```

RocksDB local table example:

```sql
>>CREATE TABLE t(a INT, b VARCHAR(20));

--- SQL operation complete.
>>INSERT INTO t VALUES (1, 'one');

--- 1 row(s) inserted.
>>SELECT * FROM t;

A  B
-  ---

1  one

--- 1 row(s) selected.
>>DROP TABLE t;

--- SQL operation complete.
>>exit;
```

To verify persistence, use the same `TRAF_LOCAL_STORE_DIR` across two separate
`sqlci` processes. Create and insert in the first process, then run
`SELECT * FROM t;` in the second process.

No `sqenv.sh`, `sqgen`, `sqstart`, monitor, DCS, REST, ZooKeeper, HBase, Hadoop,
or Java process is required for this `sqlci` path.

## Operational Usage

Use the built `sqlci` and matching local-lite SQL libraries from the same
worktree. In a development build, prefer an explicit shell setup:

```bash
cd /path/to/trafodion

export SQL_LIBS=$PWD/core/sql/lib/linux/64bit/debug
export SQF_LIBS=$PWD/core/sqf/export/lib64d
export TRAF_HOME=$PWD/core/sqf
export TRAF_LOCAL_LITE=1
export TRAF_LOCAL_STORE_DIR=${TRAF_LOCAL_STORE_DIR:-/tmp/traf-local-lite-store}
export LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS:${LD_LIBRARY_PATH:-}

$SQL_LIBS/sqlci
```

The local RocksDB catalog and table data can be removed by deleting the selected
store directory when no `sqlci` process is using it:

```bash
rm -rf "$TRAF_LOCAL_STORE_DIR"
```

If `TRAF_LOCAL_STORE_DIR` is unset, local-lite uses
`$TRAF_VAR/localstore/rocksdb` when `TRAF_VAR` is set, otherwise
`./localstore/rocksdb` relative to the process working directory.

### Troubleshooting

If `CREATE TABLE` reports a TMF error such as:

```text
ERROR[8604] Transaction subsystem TMF returned error 82 while starting a transaction.
```

then the local-lite compiler/executor table path was not selected. The usual
causes are:

- Running `sqlci` from a workspace that does not contain this implementation.
- Loading an older `libsqlcilib.so` because `LD_LIBRARY_PATH` points at another
  build first.
- Rebuilding without `TRAF_LOCAL_LITE=1`.

Check the binary and library resolution:

```bash
which sqlci || true
ldd "$SQL_LIBS/sqlci" | grep libsqlcilib
ldd "$SQL_LIBS/sqlci" | grep librocksdb
```

The resolved `libsqlcilib.so` should come from the same
`core/sql/lib/linux/64bit/debug` directory as `$SQL_LIBS/sqlci`, and `librocksdb`
should be present.

Supported local-lite `sqlci` behavior:

- SQL parsing, binding, normalization, optimization, and executor startup.
- `SELECT` queries against `VALUES` clauses.
- Minimal RocksDB local table support: `CREATE TABLE` and `DROP TABLE` route
  through the compiler DDL path; single-row and multi-row
  `INSERT INTO ... VALUES (...)` bind and execute through the local RocksDB
  executor insert TCB; local table `SELECT` statements bind through the local
  catalog NATable and run through the local RocksDB executor scan TCB. Runtime
  coverage includes projection, predicates, ordering, self-join, inner join,
  left join, ungrouped aggregate expression query shapes, and grouped
  aggregates over duplicate and NULL group keys with `HAVING`.
- Basic scalar expressions, arithmetic, and string operations.
- Direct `DATE`, `TIME`, and `TIMESTAMP` result rendering for standalone
  expressions and local table columns.
- `exit;` and `quit;`.

Unsupported behavior:

- Local table `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, `CREATE INDEX`, broad
  type coverage, constraints beyond local-lite primary/unique keys, privileges,
  and distributed transactions.
- HBase-backed metadata/storage operations.
- HDFS, HBase, Hive, ORC, bulk load/unload, and LOB storage access.
- JDBC/ODBC, DCS, REST, TrafCI, and remote client connectivity.
- Distributed query execution.
- Full transaction service behavior through TM/DTM/RMS.

## Local-Lite SQL Regress Lane

`core/sql/regress/localLite/runregr` provides a native-only regression driver
for the current single-process SQLCI runtime. It deliberately does not source
the legacy regress environment or invoke `regrinit.sql`, `runmxcmp.ksh`, TMF,
the monitor, HBase, Hadoop, or ZooKeeper. Each TEST case gets an isolated
temporary `TRAF_LOCAL_STORE_DIR`.

The lane preserves the established `TESTnnn`/`EXPECTEDnnn` naming and produces
`RAWnnn`, filtered `LOGnnn`, and unified `DIFFnnn` artifacts. Its common filter
removes SQLCI session noise, normalizes trailing whitespace, and masks dynamic
`MXID` diagnostic identifiers while retaining SQL results and errors. A
`-diff`/`--diff-only` mode can recompare existing LOG files from a selected run
directory without launching SQLCI again.

Current cases are:

- `TEST001`: supported scalar/table execution adapted from the datetime portion
  of `core/TEST038` and nullable-row coverage in `executor/TEST063`, plus local
  primary/UNIQUE keys, predicates, grouping, ordering, and a self-join.
- `TEST002`: explicit COMMIT/ROLLBACK behavior and failed primary-key COMMIT
  atomicity.
- `TEST003`: failed UNIQUE COMMIT atomicity and keyless row-id recovery.
- `TEST004`: mixed aligned executor rows with nullable VARCHAR fields, primary
  and VARCHAR UNIQUE get-row access, NUMERIC/DECIMAL/BigNum values, and
  direct DATE/TIME/TIMESTAMP result materialization from standalone expressions
  and persisted rows.

Run all cases or a selected subset from the repository root:

```bash
make local-lite-regress
make local-lite-regress LOCAL_LITE_REGR_TESTS="001 003"
```

The broad legacy `core`, `executor`, and `seabase` suites are not claimed as
compatible. They still contain unsupported UPDATE/DELETE/index/service-stack
operations and depend on generated regress tools that are absent from this
checkout. Portable SQL should be moved into this lane incrementally as the
corresponding compiler/executor/storage behavior becomes supported.

## RocksDB Local Store Implementation

This section records the implementation state of the current local-lite
RocksDB path. Keep this section current when completing the plan items below.

### Build Wiring

- `core/sql/nskgmake/Makerules.linux` defines local-lite-only `ROCKSDB_INC`,
  `ROCKSDB_LIB`, and a header probe for `rocksdb/c.h`.
- `core/sql/nskgmake/executor/Makefile` compiles
  `core/sql/localstore/LocalLiteRocksDBStore.cpp` and
  `core/sql/localstore/LocalLiteRowCodec.cpp` into the executor library for
  local-lite builds.
- `core/sql/nskgmake/sqlcilib/Makefile` compiles only
  `core/sql/sqlci/LocalLiteSqlTable.cpp` into `libsqlcilib.so` for
  local-lite builds. SQLCI does not link the RocksDB store or row codec for
  local table IO.
- `core/sql/nskgmake/sqlcomp/Makefile` compiles the RocksDB store into
  `libsqlcomp.so` for local-lite builds, so compiler DDL can write local
  catalog metadata without linking Java, HBase, or HDFS code.
- `core/sql/nskgmake/optimizer/Makefile` compiles the RocksDB store into
  `liboptimizer.so` for local-lite builds, so NATable loading can read the
  local catalog before falling back to Seabase/HBase metadata.
- `scripts/install-local-lite-deps.sh` installs `librocksdb-dev` on apt-based
  systems and `rocksdb-devel` on rpm-based systems.
- `core/sqf/Makefile` excludes `make_monitor` and top-level `tools` from
  `LOCAL_LITE_COMPONENTS`.

### Storage Layout

The local store root is selected in this order:

```text
TRAF_LOCAL_STORE_DIR
TRAF_VAR/localstore/rocksdb
./localstore/rocksdb
```

The RocksDB directories are:

```text
<root>/catalog
<root>/data/<catalog>/<schema>/<object_uid>
```

The catalog DB stores table metadata by fully qualified name and object UID.
Each table has a dedicated RocksDB DB. Heap table keys are big-endian `uint64`
row IDs. Values contain a store-level row wrapper around a `LLBR1` versioned
binary aligned row payload.

### Compiler DDL Path

`CREATE TABLE` and `DROP TABLE` now use the compiler DDL path in local-lite
builds instead of SQLCI string parsing.

- `core/sql/sqlci/LocalLiteSqlTable.cpp` no longer intercepts `CREATE TABLE`
  or `DROP TABLE`. Those statements proceed to CLI prepare/execute.
- `core/sql/generator/GenPreCode.cpp` marks local-lite table CREATE/DROP as not
  needing a transaction.
- `core/sql/generator/GenRelMisc.cpp` marks local-lite table CREATE/DROP DDL
  TDBs as HBase DDL. This reuses the existing embedded compiler
  `PROCESSDDL` executor path without starting TMF.
- `core/sql/executor/ex_ddl.cpp` initializes embedded arkcmp when needed in
  local-lite mode, restores the embedded compiler context before `PROCESSDDL`,
  and drops stale empty diagnostics after successful local DDL.
- `core/sql/arkcmp/CmpStatement.cpp` binds the direct
  `StmtDDLCreateTable`/`StmtDDLDropTable` node for local-lite table DDL and
  calls `CmpSeabaseDDL::executeSeabaseDDL()` directly. This avoids the full
  `DDLExpr::bindNode()` wrapper path that expects initialized Seabase/HBase
  services. Local-lite DDL business errors are returned as embedded compiler
  diagnostics on the SUCCESS return path because the ERROR return path is
  wrapped by the generic compiler-server failure before SQLCI displays the
  original diagnostic.
- `core/sql/sqlcomp/CmpSeabaseDDLcommon.cpp` recognizes local-lite local table
  DDL, skips the uninitialized Seabase/HBase guard, skips
  `sendAllControlsAndFlags()`, suppresses DDL transaction start, and dispatches
  CREATE TABLE directly to the local table branch.
- `core/sql/sqlcomp/CmpSeabaseDDLtable.cpp` implements local-lite
  `localLiteCreateTable()` and `localLiteDropTable()` helpers. They apply
  catalog/schema defaults, reject unsupported constraints, defaults, physical
  attributes, Hive options, LOB-like types, and `DROP TABLE CASCADE`, and then
  write or remove table metadata through `LocalLiteRocksDBStore`.

The acceptance symptom fixed by this path is:

```text
ERROR[8604] Transaction subsystem TMF returned error 82 while starting a transaction.
```

Local-lite table DDL should now complete without TMF, Seabase metadata bootstrap,
HBase, Hadoop, ZooKeeper, or Java runtime services.

### Local Catalog NATable Loading

`core/sql/optimizer/NATable.cpp` now has a `TRAF_LOCAL_LITE` metadata path for
local RocksDB tables. During `NATableDB::get()`, local-lite checks
`LocalLiteRocksDBStore` before calling `CmpSeabaseDDL::getSeabaseTableDesc()`
for regular Seabase table names. If the table exists in the local catalog, the
optimizer synthesizes a minimal `TrafDesc` tree on the NATable heap:

- `DESC_TABLE_TYPE` with the local object UID, aligned row format, no
  partitioning, regular insert mode, and droppable table flag.
- Audited table and index `DESC_FILES_TYPE` nodes, so normal DML binding does
  not reject the table as non-audited before it reaches the local-lite executor
  guard.
- User column descriptors from catalog column metadata.
- A minimal primary index descriptor and key descriptor, reusing the existing
  HBase/Seabase scan TDB shape as the carrier for the local RocksDB scan TCB.
  Keyless tables expose the internal RocksDB row id as a hidden `SYSKEY`
  clustering column instead of treating the first user column as unique.
- Logical UNIQUE access-path descriptors for local-lite `UNIQUE` metadata. These
  descriptors expose unique keys to the optimizer but keep the physical
  `indexname` on the base local table because local-lite stores `U` uniqueness
  records in the same RocksDB table rather than in separate index tables.

The current type mapper covers the local DDL v1 scalar surface needed for
binding: signed tiny/small/int/large integers, real/float, double, `CHAR`,
`VARCHAR`, `DATE`, `TIME`, and `TIMESTAMP`. Character columns are represented as
ISO88591 descriptors.

`core/sql/sqlci/LocalLiteSqlTable.cpp` no longer intercepts local table
`SELECT`. All supported queries, including `VALUES` queries and local table
queries, fall through to CLI prepare and executor execution. Local table scans
bind through the local catalog NATable path and execute through
`LocalLiteHbaseScanTcb`.

### Executor Scan TCB

`core/sql/executor/LocalLiteStorageStubs.cpp` now builds
`LocalLiteHbaseScanTcb` for local-lite `ExHbaseAccessTdb::SELECT_` plans. The
TCB loads table metadata and rows from `LocalLiteRocksDBStore`, uses
`LocalLiteRowCodec` to project the persisted binary aligned payload through the
fetched-column list, writes those values into the compiler-generated binary
aligned row descriptor, and returns rows through the normal executor up queue.
Encoded local-lite get-row requests are consumed through
`LocalLiteTxn::getRowByKey()`. Primary-key requests use deterministic `P`
records. UNIQUE-key requests use deterministic `U` records and resolve back to
the base persisted `LLBR1` row before projection.

This replaces the previous SQLCI `SELECT *` bypass.

### Executor Insert TCB

`core/sql/executor/LocalLiteStorageStubs.cpp` also builds
`LocalLiteHbaseInsertTcb` for local-lite `ExHbaseAccessTdb::INSERT_` plans. The
TCB evaluates the compiler-generated `convertExpr_`, then
`LocalLiteRowCodec` normalizes that executor-produced row into the local
canonical binary aligned table layout and persists the `LLBR1` payload through
`LocalLiteRocksDBStore`.
In local-lite mode the binder marks HBase-style DML as not needing generic index
maintenance; the local storage layer maintains local `U` uniqueness records for
UNIQUE constraints in the base RocksDB table.

This replaces the previous SQLCI `INSERT INTO ... VALUES` bypass. INSERT type
conversion and expression evaluation now come from the compiler/executor path
for the supported v1 local table types.

### SQLCI Handler

`core/sql/sqlci/SqlCmd.cpp` calls
`LocalLiteSqlTable_process()` before CLI prepare in local-lite builds. The
handler recognizes:

- `CREATE INDEX`, `CREATE VIEW`, `CREATE SEQUENCE`, `CREATE SCHEMA`,
  `CREATE SYNONYM`, `ALTER TABLE`, `TRUNCATE TABLE`, `UPDATE`, `DELETE`,
  `MERGE`, and `UPSERT` as explicit unsupported statements
- Native HBase/Hive/volatile table DDL as explicit unsupported statements
- Table constraints/default/generated/identity column definitions as explicit
  unsupported syntax

Unqualified table names default to `TRAFODION.SEABASE.<name>`. Unquoted
identifiers are uppercased; quoted identifiers preserve case.

`CREATE TABLE`, `DROP TABLE`, `INSERT`, and `SELECT` intentionally fall through
this handler and use the compiler/executor paths described above.

All supported query execution must use executor TCBs. The SQLCI local-lite hook
is only an early unsupported-statement gate and must not scan RocksDB rows,
insert RocksDB rows, or materialize result rows directly.

### Executor Guard

`core/sql/executor/LocalLiteStorageStubs.cpp` still builds
`LocalLiteUnsupportedHbaseTcb` for HBase access TDB plans other than local-lite
`SELECT_` and `INSERT_` instead of returning `NULL`. This prevents a vague
crash if an old unsupported HBase access TDB path is reached.

The unsupported TCB also implements the standard private-state allocator, so
unsupported compiled storage plans can emit a diagnostic instead of tripping a
generic executor queue assertion.

## RocksDB Local Store Plan

Use this list as the implementation tracker. When a task is completed, change
its checkbox to `[x]` and add the implementation details to the relevant section
above.

### Current Task Status

Last updated after fixing direct datetime result materialization in standalone
local-lite SQLCI.

Completed:

- RocksDB dependency detection and local-lite link flags.
- Native RocksDB catalog/table store module.
- Explicit unsupported diagnostics for known local-lite unsupported SQL.
- HBase access TDB guard that returns an unsupported TCB instead of `NULL`.
- Local-lite SQF monitor/tools build trimming.
- SQLCI-entry RocksDB smoke/regression coverage for compiler/executor-backed
  table IO.
- Compiler-routed local table `CREATE TABLE` and `DROP TABLE`, including TMF
  avoidance and visible compiler DDL diagnostics.
- Local catalog NATable loading for compiler-bound local table references.
- Local RocksDB executor scan TCB for compiler-bound local table SELECTs.
- Local RocksDB executor insert TCB for compiler-bound local table INSERTs.
- Binary aligned executor row materialization and fetched-column projection
  mapping for local table SELECTs.
- Versioned binary aligned row persistence for executor INSERT values.
- Predicate evaluation for local RocksDB scans, including predicates on columns
  that are not part of the final projection.
- Date/time/timestamp local table rows through executor insert, `LLBR1`
  persistence, and executor scan predicates.
- Small exact `NUMERIC(p,s)` local table rows through executor insert,
  `LLBR1` persistence, and executor scan predicates.
- `DECIMAL(p,s)` and BigNum `NUMERIC(p,s)` local table rows through executor
  insert, `LLBR1` persistence, and executor scan predicates.
- NULL and insert-expression edge cases for local executor INSERT/SCAN rows.
- v1 unsupported object/type rules split between SQLCI pre-prepare checks and
  compiler DDL checks.
- Operational usage documentation for the current sqlci entry point and
  compiler/executor-backed RocksDB table path.
- Local store public API cleanup that prevents local executor writes from
  bypassing `LocalLiteTxn::insertRow()`.
- Same-process local store concurrency probe covering multiple writer threads,
  overlapping scans, shared RocksDB handles, and duplicate-free row-id
  allocation through the transaction facade.
- Cross-process shared-store boundary enforcement for `TRAF_LOCAL_STORE_DIR`:
  a second process that tries to open an already-held local store receives an
  explicit local-lite diagnostic instead of a raw RocksDB `LOCK` message.
- Broader executor expression smoke coverage for local table INSERT/SCAN paths,
  including `CASE`, string concatenation, `BETWEEN`, `IN`, `LIKE`, and `OR`
  predicates.
- Broader local table query-shape smoke coverage for executor scan TCBs,
  including `ORDER BY`, self-join, inner join, and left join.
- Ungrouped aggregate expression smoke coverage for local table scans,
  including `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, aggregate arithmetic, and
  aggregate input filtering over NULL values.
- Grouped aggregate correctness for local table scans, including duplicate
  group keys, NULL group keys, nullable UNIQUE group keys, aggregate input
  filtering, and `HAVING` predicates evaluated above the aggregate.
- Keyless NATable/NAFileSet metadata exposes the synthetic RocksDB row identity
  as a hidden `SYSKEY` clustering key. Executor scans materialize that value
  from `LocalLiteRow::rowId`, executor inserts ignore the generated SYSKEY
  placeholder when building `LLBR1`, and normal groupby elimination rules can
  rely on optimizer key metadata without local-lite rule guards.
- Explicit transaction COMMIT preflights all pending primary and UNIQUE keys
  against committed storage, then publishes each table's base rows and
  secondary uniqueness records with one RocksDB write batch. Commit-failure
  regressions verify that a later duplicate cannot partially publish an earlier
  row from the same table and that a failed keyless UNIQUE commit does not
  advance persisted row-id metadata.
- A native `core/sql/regress/localLite` lane runs isolated TEST/EXPECTED cases
  directly through the built local-lite SQLCI. It produces RAW/LOG/DIFF
  artifacts, supports selecting individual case numbers, and requires no SQF,
  TMF, HBase, Hadoop, or ZooKeeper service startup.
- Executor INSERT normalization resolves the actual length-indicator location
  of every aligned VARCHAR through its VOA entry. This prevents the second and
  later variable columns in a mixed wide row from being mistaken for a
  truncated executor tuple. Native `TEST004` covers reverse-order variable
  projection, NULL variable fields, primary/UNIQUE lookups, numeric families,
  and datetime fields in one table.
- The local-lite SQLCI prologue enables CLI internal-format IO before preparing
  the first user statement. SQLCI Formatter therefore receives binary datetime
  values instead of external text that would be converted a second time.
  Native `TEST004` validates direct DATE/TIME/TIMESTAMP output from both
  `VALUES` and persisted local rows without explicit character casts.

Remaining, in suggested implementation order:

1. Port more compatible `core` and `executor` SQL into the native regress lane.

The next task to start is **Broader native regress SQL coverage**.

- [x] **Build RocksDB dependency detection and link flags.**
  - Implemented in `core/sql/nskgmake/Makerules.linux`.
  - Verified by `scripts/test-local-lite-runtime.sh` and `make local-lite`.

- [x] **Add native RocksDB catalog/table store module.**
  - Implemented in `core/sql/localstore/LocalLiteRocksDBStore.h` and
    `core/sql/localstore/LocalLiteRocksDBStore.cpp`.
  - Current store supports create, drop, metadata lookup, row ID allocation,
    row insert, and full row scan.

- [x] **Remove SQLCI direct local table IO and route supported statements to
  compiler/executor paths.**
  - Implemented in `core/sql/sqlci/LocalLiteSqlTable.cpp` and
    `core/sql/sqlci/SqlCmd.cpp`.
  - SQLCI no longer performs direct RocksDB table scans or inserts. The
    local-lite SQLCI hook only reports known unsupported statements before
    prepare.
  - `CREATE TABLE` and `DROP TABLE` were moved out of this SQLCI handler and
    now use the compiler DDL path.
  - `INSERT` and all table `SELECT` queries were also moved out of this SQLCI
    handler and now use executor TCBs after normal CLI prepare/bind/generate.

- [x] **Return explicit unsupported diagnostics for known unsupported local
  table SQL.**
  - Implemented for `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, and `CREATE INDEX`
    in `core/sql/sqlci/LocalLiteSqlTable.cpp`.

- [x] **Prevent old HBase access TDB build from returning `NULL`.**
  - Implemented as `LocalLiteUnsupportedHbaseTcb` in
    `core/sql/executor/LocalLiteStorageStubs.cpp`.

- [x] **Avoid building unsupported SQF monitor/tools targets in local-lite.**
  - Implemented by removing `make_monitor` and top-level `tools` from
    `LOCAL_LITE_COMPONENTS` in `core/sqf/Makefile`.

- [x] **Add automated smoke coverage for the current sqlci-entry RocksDB
  path.**
  - Implemented in `scripts/test-local-lite-rocksdb-sqlci.sh`.
  - The script verifies `librocksdb` linkage, absence of Java/Hadoop/HBase/
    ZooKeeper dynamic library names, local table create/insert/select/drop, and
    persistence across two `sqlci` processes.

- [x] **Move local table DDL from SQLCI string parsing into the compiler DDL
  path.**
  - Implemented in `core/sql/sqlci/LocalLiteSqlTable.cpp`,
    `core/sql/generator/GenPreCode.cpp`, `core/sql/generator/GenRelMisc.cpp`,
    `core/sql/executor/ex_ddl.cpp`, `core/sql/arkcmp/CmpStatement.cpp`,
    `core/sql/sqlcomp/CmpSeabaseDDLcommon.cpp`, and
    `core/sql/sqlcomp/CmpSeabaseDDLtable.cpp`.
  - CREATE/DROP now fall through SQLCI, execute through embedded compiler
    `PROCESSDDL`, avoid TMF startup, and write/drop RocksDB catalog metadata
    from sqlcomp.
  - Duplicate CREATE, unsupported constraints/defaults, unsupported LOB-like
    types, and unsupported DROP CASCADE are compiler DDL diagnostics.
  - Embedded compiler context restoration and SUCCESS-with-error-diagnostics
    propagation keep local-lite DDL errors visible in SQLCI instead of being
    masked by generic `-2013`/`-8822` compiler-server errors.

- [x] **Add NATable loading from the local catalog.**
  - Implemented in `core/sql/optimizer/NATable.cpp` with local-lite-only
    catalog lookup and synthesized `TrafDesc` table/column/index descriptors.
  - `core/sql/nskgmake/optimizer/Makefile` now compiles
    `LocalLiteRocksDBStore.cpp` into `liboptimizer.so` for local-lite builds.
  - `core/sql/sqlci/LocalLiteSqlTable.cpp` now leaves table SELECT queries for
    the normal compiler path instead of handling them in the SQLCI bypass.
  - `core/sql/executor/LocalLiteStorageStubs.cpp` allocates queue private state
    for the unsupported HBase TCB so compiled local table plans return a clear
    unsupported diagnostic instead of asserting.
  - Acceptance check: `SELECT * FROM t` against a local RocksDB table no longer
    reports table-not-found or SQLCI bypass errors; it binds through NATable.

- [x] **Replace the SQLCI `SELECT *` bypass with an executor TCB for local table
  scans.**
  - Implemented in `core/sql/executor/LocalLiteStorageStubs.cpp` as
    `LocalLiteHbaseScanTcb`, using the existing HBase access TDB shape only as
    a carrier for local table scan metadata.
  - `core/sql/comexe/ComTdbHbaseAccess.h` grants the local-lite scan TCB access
    to the compiler-generated work CRI, tuple indexes, and expressions without
    changing the TDB layout.
  - Acceptance check: generated table access plans instantiate the local
    RocksDB scan TCB and return rows through normal executor queues.

- [x] **Materialize projected scan rows in the binary aligned executor row
  layout.**
  - Implemented in `core/sql/generator/GenRelScan.cpp`,
    `core/sql/optimizer/NATable.cpp`, and
    `core/sql/executor/LocalLiteStorageStubs.cpp`.
  - Local NATable column descriptors now carry synthetic HBase qualifiers so
    the existing HBase access TDB can expose a fetched-column list for local
    table columns.
  - The generator keeps projected base/index columns mapped to the scan tuple
    attributes needed by the local-lite TCB.
  - The scan TCB maps fetched columns from the persisted binary aligned row
    payload to the requested projection and writes supported values into the
    compiler-generated binary aligned row descriptor.
  - Runtime validation covers `SELECT b`, `SELECT a, b`, and `SELECT *` for
    an `INT, VARCHAR` local table.

- [x] **Persist local table rows as versioned binary aligned payloads.**
  - Implemented in `core/sql/localstore/LocalLiteRowCodec.cpp` and
    `core/sql/localstore/LocalLiteRowCodec.h`.
  - Executor `INSERT INTO ... VALUES` now evaluates the compiler-generated
    insert expression and persists the complete table row as `LLBR1` binary
    aligned row data through `LocalLiteRocksDBStore`.
  - Executor scans project directly from the binary payload; the older
    SQLCI-local text-field row payload is no longer used for new rows.
  - Runtime validation covers persisted `INT, VARCHAR` rows across separate
    `sqlci` processes.

- [x] **Move local table inserts to executor expressions.**
  - Implemented in `core/sql/executor/LocalLiteStorageStubs.cpp` as
    `LocalLiteHbaseInsertTcb`.
  - `core/sql/sqlci/LocalLiteSqlTable.cpp` no longer intercepts `INSERT`.
  - `core/sql/generator/GenRelMisc.cpp` disables root transaction startup in
    local-lite, since this runtime does not start TMF.
  - `core/sql/localstore/LocalLiteRowCodec.cpp` normalizes the
    executor-produced insert row into the local canonical binary aligned row
    layout before persistence.
  - Runtime validation covers single-row and multi-row `INSERT INTO ... VALUES`
    followed by projected executor scans.

- [x] **Harden predicates for local RocksDB scans.**
  - Ensure predicate-only columns are available to expression evaluation even
    when they are not part of the final projection.
  - Implemented in `core/sql/executor/LocalLiteStorageStubs.cpp` by
    materializing the scan ascii tuple from persisted `LLBR1` rows, evaluating
    the compiler-generated `scanExpr_`, and only then materializing the
    projected convert tuple.
  - `core/sql/generator/GenRelScan.cpp` adds executor-predicate columns before
    projection-only columns in the local-lite fetched-column list so predicate
    expressions receive the correct tuple attributes.
  - Acceptance check: `SELECT b FROM t WHERE a = 1;` runs through the normal
    SQL compiler/executor path and returns only matching projected columns.

- [x] **Extend local-lite scalar type coverage for datetime types.**
  - Implemented in `core/sql/localstore/LocalLiteRowCodec.cpp`.
  - `DATE`, `TIME`, and `TIMESTAMP` values are persisted from the
    executor-produced binary representation and copied back into executor scan
    rows as `REC_DATETIME` values.
  - Runtime validation covers executor inserts and scan predicates over
    `DATE`, `TIME`, and `TIMESTAMP` columns while projecting a separate
    `VARCHAR` column.

- [x] **Extend local-lite scalar type coverage for small exact NUMERIC types.**
  - Implemented in `core/sql/sqlcomp/CmpSeabaseDDLtable.cpp`,
    `core/sql/optimizer/NATable.cpp`, and
    `core/sql/localstore/LocalLiteRowCodec.cpp`.
  - `NUMERIC(p,s)` values with precision `1..18` are stored as fixed binary
    executor values in the local `LLBR1` row; write-side encoding still comes
    from the compiler-generated insert expression.
  - Runtime validation covers executor inserts and scan predicates over
    `NUMERIC(5,2)` while projecting a separate `VARCHAR` column.

- [x] **Extend local-lite scalar type coverage for DECIMAL and BigNum NUMERIC
  types.**
  - Implemented in `core/sql/sqlcomp/CmpSeabaseDDLtable.cpp`,
    `core/sql/optimizer/NATable.cpp`, and
    `core/sql/localstore/LocalLiteRowCodec.cpp`.
  - `DECIMAL(p,s)` uses Trafodion's `REC_DECIMAL_*` physical representation,
    and `NUMERIC(p,s)` with precision above 18 uses Trafodion's
    `REC_NUM_BIG_*` BigNum physical representation.
  - Local-lite persists both by copying executor-produced bytes into the
    canonical `LLBR1` binary aligned row and projecting those bytes back into
    executor scan rows.
  - Runtime validation covers executor inserts and scan predicates over
    `DECIMAL(5,2)` and `NUMERIC(30,2)` while projecting a separate `VARCHAR`
    column.

- [x] **Cover NULL and expression edge cases for local executor INSERT/SCAN
  rows.**
  - Implemented in `core/sql/localstore/LocalLiteRowCodec.cpp` by preserving
    NULL bitmap state and advancing variable-column VOA state when projecting
    stored nullable VARCHAR values into executor scan rows.
  - `core/sql/executor/LocalLiteStorageStubs.cpp` now uses the variable column
    indicator width when writing local-lite variable-column VOA entries.
  - Runtime validation covers NULL fixed-width predicates, NULL VARCHAR
    predicates, executor insert expressions such as arithmetic and CAST,
    NOT NULL insert diagnostics, and multiple nullable VARCHAR columns.

- [x] **Define and enforce v1 unsupported object/type rules before CLI
  prepare.**
  - Implemented in `core/sql/sqlci/LocalLiteSqlTable.cpp` for statements that
    still use the SQLCI pre-prepare bypass, and in
    `core/sql/sqlcomp/CmpSeabaseDDLtable.cpp` for compiler-routed local table
    DDL.
  - Current checks cover Hive/HBase/volatile table DDL, LOB-like column types,
    indexes, views, sequences, schemas, synonyms, table constraints/defaults/
    generated/identity columns, `ALTER TABLE`, `TRUNCATE TABLE`, `UPDATE`,
    `DELETE`, `MERGE`, and `UPSERT`.
  - The DDL-related rules for local `CREATE TABLE` and `DROP TABLE` now live in
    the compiler path, so they are enforced before reaching HBase, HDFS, Java,
    or TMF code.

- [x] **Add a focused regression test suite for local-lite RocksDB through
  sqlci.**
  - Implemented by extending `scripts/test-local-lite-rocksdb-sqlci.sh`.
  - Current coverage includes create/drop, duplicate table errors, missing
    table errors, insert column count errors, unsupported LOB type diagnostics,
    multi-row inserts, restart persistence, unsupported SQL, `librocksdb`
    linkage, and Java/Hadoop/HBase/ZooKeeper dynamic dependency checks.

- [x] **Document operational usage for the current sqlci-entry RocksDB path.**
  - Added environment setup, supported SQL examples, data directory cleanup, and
    troubleshooting for accidentally running an old `sqlci` or old
    `libsqlcilib.so`.
  - Revisit this section after the full compiler/NATable/executor table path is
    stable.

## Legacy Scripts

The standalone `sqlci` path does not need shell scripts. The scripts still exist
for full Trafodion workflows and for compatibility with existing development
habits.

Current local-lite-specific script behavior:

- `sqenvcom.sh` sets local-lite config defaults, clears Hadoop/HBase classpath
  setup, and avoids Hadoop/HBase distro probing when `TRAF_LOCAL_LITE=1`.
- `sqgen` skips HBase classpath cache cleanup when `TRAF_LOCAL_LITE=1`.
- `sqstart` skips Kerberos/Hadoop checks, distributed HBase cleanup,
  ZooKeeper cleanup, and `hbcheck` when `TRAF_LOCAL_LITE=1`.

These guards do not make the legacy SQF service stack a supported standalone
database mode. They only prevent local-lite development workflows from failing
early on intentionally absent Hadoop/HBase/ZooKeeper dependencies.

## Disabled Storage Behavior

HDFS, Hive, HBase, ORC, bulk-load, LOB, and related paths are intentionally
disabled. Local-lite code should fail explicitly if one of those paths is
reached.

Current examples:

- Optimizer HDFS hooks record local-lite unavailable diagnostics.
- Hive metadata, Hive truncate, and Hive query execution utilities emit
  unsupported diagnostics.
- HDFS/HBase bulk unload paths emit unsupported diagnostics.
- HBase/HDFS executor and interface compatibility symbols exist only to keep the
  native build/link path complete.

Silent success for disabled storage work is a bug.

## Verification

Check the local-lite dry run for disabled Java/Hadoop build modules:

```bash
make -n local-lite | egrep 'mvn|javac|hbase-trx|hbasetmlib2|hbase_utilities|JAVA_HOME'
```

Expected output: no matching lines.

After a successful local-lite link, check produced binaries and libraries for
JVM or HDFS dynamic dependencies:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected output: no matching lines.

For `sqlci`, run `ldd` with the local-lite library path:

```bash
SQL_LIBS=$(pwd)/core/sql/lib/linux/64bit/debug
SQF_LIBS=$(pwd)/core/sqf/export/lib64d
LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS ldd $SQL_LIBS/sqlci | egrep -i 'jvm|java|hdfs|hbase|zookeeper'
```

Expected output: no matching lines.

Runtime smoke tests used during development:

```bash
TRAF_HOME=$(pwd)/core/sqf TRAF_LOCAL_LITE=1 LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS $SQL_LIBS/sqlci -v
printf 'exit;\n' | TRAF_HOME=$(pwd)/core/sqf TRAF_LOCAL_LITE=1 LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS $SQL_LIBS/sqlci
printf 'SELECT 1 FROM (VALUES(1)) AS t(x);\nexit;\n' | TRAF_HOME=$(pwd)/core/sqf TRAF_LOCAL_LITE=1 LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS $SQL_LIBS/sqlci
```

Current local-lite static and RocksDB SQLCI smoke checks:

```bash
bash scripts/test-local-lite-runtime.sh
bash scripts/test-local-lite-rocksdb-sqlci.sh
make local-lite-regress
```

## Design Rules

- Keep the full Trafodion build unchanged unless `TRAF_LOCAL_LITE=1` is set.
- Keep all local-lite behavior compile-time gated by `TRAF_LOCAL_LITE`.
- Prefer small native stubs over compiling Java/Hadoop-backed code.
- Keep the RocksDB local store native-only and single-process until the
  compiler, executor, and transaction boundaries are explicitly designed.
- Disabled HDFS/Hive/HBase paths must fail explicitly.
- Treat `sqlci` standalone as the first supported runtime milestone; do not
  imply `mxosrvr`, DCS, REST, or the full SQF service stack is standalone.
