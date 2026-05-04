# Trafodion Local-Lite

## Overview

`local-lite` is a native-only Trafodion build and runtime mode for local SQL
engine development. It removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS,
REST, TrafCI, and Java client modules from the selected build path.

The current useful runtime target is `sqlci` in local-lite mode. It can start as
a single local process and execute SQL that stays inside the compiler/executor,
for example `SELECT` statements over `VALUES` clauses.

Local-lite is not a complete standalone database yet. It now includes a minimal
RocksDB-backed local table path for `sqlci` table smoke tests, but the full
Trafodion catalog, NATable loading path, optimizer integration, executor TCBs,
transactions, indexes, privileges, JDBC/ODBC connectivity, and distributed
execution remain outside the supported runtime surface.

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
- Minimal `sqlci` local table handler compiled into `libsqlcilib.so` only when
  `TRAF_LOCAL_LITE=1`.
- Minimal RocksDB catalog and table data store under
  `TRAF_LOCAL_STORE_DIR` or `TRAF_VAR/localstore/rocksdb`.
- Basic local table SQL in `sqlci`: compiler-routed `CREATE TABLE` and
  `DROP TABLE`, plus SQLCI-local `INSERT INTO ... VALUES (...)`, multi-tuple
  `INSERT INTO ... VALUES (...), (...)`, and `SELECT * FROM <table>`.
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
  `CREATE TABLE` and `DROP TABLE`, but local table DML and scans are still an
  SQLCI-local bypass. It does not yet use Trafodion's NATable loader, optimizer
  table descriptors, or executor row-expression machinery.
- Local table rows are stored as text field values encoded by the SQLCI handler.
  Type enforcement and expression evaluation are not yet equivalent to
  Trafodion table execution.
- `SELECT` over local RocksDB tables currently supports only
  `SELECT * FROM <table>` without predicates, projection lists, joins,
  grouping, ordering, or expressions.
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

A B
- -

1 one

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

then SQLCI did not use the local-lite RocksDB handler. The usual causes are:

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
  `INSERT INTO ... VALUES (...)` and `SELECT * FROM <table>` are still handled
  by the SQLCI-local table path.
- Basic scalar expressions, arithmetic, and string operations.
- `exit;` and `quit;`.

Unsupported behavior:

- Local table `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, `CREATE INDEX`,
  predicates, projection lists, joins, grouping, ordering, expression
  evaluation, type coercion, constraints, privileges, and transactions.
- HBase-backed metadata/storage operations.
- HDFS, HBase, Hive, ORC, bulk load/unload, and LOB storage access.
- JDBC/ODBC, DCS, REST, TrafCI, and remote client connectivity.
- Distributed query execution.
- Full transaction service behavior through TM/DTM/RMS.

## RocksDB Local Store Implementation

This section records the implementation state as of the current
`local-lite-rocksdb` branch. Keep this section current when completing the plan
items below.

### Build Wiring

- `core/sql/nskgmake/Makerules.linux` defines local-lite-only `ROCKSDB_INC`,
  `ROCKSDB_LIB`, and a header probe for `rocksdb/c.h`.
- `core/sql/nskgmake/executor/Makefile` compiles
  `core/sql/localstore/LocalLiteRocksDBStore.cpp` into the executor library for
  local-lite builds.
- `core/sql/nskgmake/sqlcilib/Makefile` compiles
  `core/sql/sqlci/LocalLiteSqlTable.cpp` and the RocksDB store into
  `libsqlcilib.so` for local-lite builds.
- `core/sql/nskgmake/sqlcomp/Makefile` compiles the RocksDB store into
  `libsqlcomp.so` for local-lite builds, so compiler DDL can write local
  catalog metadata without linking Java, HBase, or HDFS code.
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
row IDs. Values currently contain a simple version byte, a big-endian payload
length, and the SQLCI-local encoded field payload.

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

### SQLCI Handler

`core/sql/sqlci/SqlCmd.cpp` calls
`LocalLiteSqlTable_process()` before CLI prepare in local-lite builds. The
handler recognizes:

- `INSERT INTO <name> VALUES (...)`
- `INSERT INTO <name> VALUES (...), (...)`
- `SELECT * FROM <name>`
- `CREATE INDEX`, `CREATE VIEW`, `CREATE SEQUENCE`, `CREATE SCHEMA`,
  `CREATE SYNONYM`, `ALTER TABLE`, `TRUNCATE TABLE`, `UPDATE`, `DELETE`,
  `MERGE`, and `UPSERT` as explicit unsupported statements
- Native HBase/Hive/volatile table DDL as explicit unsupported statements
- Table constraints/default/generated/identity column definitions as explicit
  unsupported syntax

Unqualified table names default to `TRAFODION.SEABASE.<name>`. Unquoted
identifiers are uppercased; quoted identifiers preserve case.

`CREATE TABLE` and `DROP TABLE` intentionally fall through this handler and use
the compiler DDL path described above.

### Executor Guard

`core/sql/executor/LocalLiteStorageStubs.cpp` now builds
`LocalLiteUnsupportedHbaseTcb` for `ExHbaseAccessTdb::build()` in local-lite
instead of returning `NULL`. This prevents a vague crash if an old HBase access
TDB path is reached before the full RocksDB executor path exists.

## RocksDB Local Store Plan

Use this list as the implementation tracker. When a task is completed, change
its checkbox to `[x]` and add the implementation details to the relevant section
above.

### Current Task Status

Last updated after commit `96f55bcaa` on the `local-lite-rocksdb` branch.

Completed:

- RocksDB dependency detection and local-lite link flags.
- Native RocksDB catalog/table store module.
- SQLCI local table DML/scan bypass for `INSERT INTO ... VALUES (...)` and
  `SELECT * FROM <table>`.
- Explicit unsupported diagnostics for known local-lite unsupported SQL.
- HBase access TDB guard that returns an unsupported TCB instead of `NULL`.
- Local-lite SQF monitor/tools build trimming.
- SQLCI RocksDB smoke/regression coverage.
- Compiler-routed local table `CREATE TABLE` and `DROP TABLE`, including TMF
  avoidance and visible compiler DDL diagnostics.
- v1 unsupported object/type rules split between SQLCI pre-prepare checks and
  compiler DDL checks.
- Operational usage documentation for the current SQLCI/RocksDB path.

Remaining, in suggested implementation order:

1. Add NATable loading from the local catalog.
2. Replace the SQLCI `SELECT *` bypass with a local RocksDB executor scan TCB.
3. Use Trafodion expression evaluation and a binary row layout for local table
   inserts/scans.
4. Implement predicates and projection for local RocksDB scans.

The next task to start is **Add NATable loading from the local catalog**.

- [x] **Build RocksDB dependency detection and link flags.**
  - Implemented in `core/sql/nskgmake/Makerules.linux`.
  - Verified by `scripts/test-local-lite-runtime.sh` and `make local-lite`.

- [x] **Add native RocksDB catalog/table store module.**
  - Implemented in `core/sql/localstore/LocalLiteRocksDBStore.h` and
    `core/sql/localstore/LocalLiteRocksDBStore.cpp`.
  - Current store supports create, drop, metadata lookup, row ID allocation,
    row insert, and full row scan.

- [x] **Wire SQLCI local table statements to RocksDB.**
  - Implemented in `core/sql/sqlci/LocalLiteSqlTable.cpp` and
    `core/sql/sqlci/SqlCmd.cpp`.
  - Current SQLCI-local support is single-row and multi-row
    `INSERT INTO ... VALUES (...)` and `SELECT * FROM <table>`.
  - `CREATE TABLE` and `DROP TABLE` were moved out of this SQLCI handler and
    now use the compiler DDL path.

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

- [x] **Add automated smoke coverage for the current SQLCI RocksDB path.**
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

- [ ] **Add NATable loading from the local catalog.**
  - Target files to investigate first: `core/sql/sqlcat/TrafDesc.cpp`,
    `core/sql/sqlcat/desc.h`, `core/sql/sqlcomp/CmpSeabaseDDL.h`, and the
    NATable construction path in `core/sql/sqlcomp`.
  - Acceptance check: local-lite can bind a local RocksDB table through the
    regular compiler metadata path instead of the SQLCI pre-prepare bypass.

- [ ] **Replace the SQLCI `SELECT *` bypass with an executor TCB for local table
  scans.**
  - Target files to investigate first:
    `core/sql/executor/ExHbaseAccess.cpp`,
    `core/sql/executor/LocalLiteStorageStubs.cpp`,
    `core/sql/comexe/ComTdbHbaseAccess.h`, and
    `core/sql/generator/GenRelScan.cpp`.
  - Acceptance check: generated table access plans instantiate a local RocksDB
    scan TCB and return rows through normal executor queues.

- [ ] **Use Trafodion expression evaluation and binary row layout for local
  table inserts/scans.**
  - Target files to investigate first: `core/sql/exp`, `core/sql/comexe`, and
    row conversion code used by existing HBase access.
  - Acceptance check: integer, decimal, floating point, `CHAR`, `VARCHAR`,
    `DATE`, `TIME`, and `TIMESTAMP` values are encoded with a versioned binary
    row format and decoded through executor expressions.

- [ ] **Implement predicates and projection for local RocksDB scans.**
  - Depends on the local scan TCB and expression-backed row format.
  - Acceptance check: `SELECT a FROM t WHERE a = 1;` runs through the normal
    SQL compiler/executor path and returns only matching projected columns.

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

- [x] **Add a focused regression test suite for local-lite RocksDB SQLCI.**
  - Implemented by extending `scripts/test-local-lite-rocksdb-sqlci.sh`.
  - Current coverage includes create/drop, duplicate table errors, missing
    table errors, insert column count errors, unsupported LOB type diagnostics,
    multi-row inserts, restart persistence, unsupported SQL, `librocksdb`
    linkage, and Java/Hadoop/HBase/ZooKeeper dynamic dependency checks.

- [x] **Document operational usage for the current SQLCI RocksDB path.**
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
