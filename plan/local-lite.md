# Trafodion Local-Lite

## Overview

`local-lite` is a native-only Trafodion build and runtime mode for local SQL
engine development. It removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS,
REST, TrafCI, and Java client modules from the selected build path.

The runtime targets are standalone `sqlci` and the long-running M11
`nativelite-server`. Both exercise the normal compiler/executor over an embedded
RocksDB catalog and table store. The bounded local surface includes transactions,
DDL/DML, primary/secondary indexes, constraints, metadata/statistics,
authorization, a constrained UDR adapter, independent per-session transaction
contexts, and a reduced Trafodion Type 4 client endpoint.

Local-lite is not a complete standalone database yet. M11 establishes the
multi-session process and protocol boundary, while M12 adds the backend-neutral
transactional storage/recovery contract. M13 makes the format-version-2
TransactionDB catalog/table key space exclusive and rejects old per-table
stores; no old-layout migration or runtime fallback is retained.
Independent session requests compile and execute on their connection threads;
DDL/catalog mutation and SQLCI compatibility utilities retain narrow locks.
The current implementation is a single-node service. Distributed
execution, password/TLS authentication, full
Trafodion wire compatibility, and the HBase/HDFS/Hive service stack
remain outside the runtime.
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
- A `sqlci` local control layer compiled into `libsqlcilib.so` only when
  `TRAF_LOCAL_LITE=1`. It handles local session/catalog compatibility commands
  and bounded metadata/authorization/UDR operations; normal table queries and
  DML continue through compiler/executor plans.
- Minimal RocksDB catalog and table data store under
  `TRAF_LOCAL_STORE_DIR` or `TRAF_VAR/localstore/rocksdb`.
- Basic local table SQL in `sqlci`: compiler-routed `CREATE TABLE`,
  `DROP TABLE`, and executor-routed single-row and multi-row
  `INSERT INTO ... VALUES (...)` plus `INSERT ... SELECT` tuple flows.
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
- Autocommit tuple flows whose target is `LocalLiteHbaseInsertTcb` open a local
  transaction before requesting source rows. They publish the pending rows in
  one per-table RocksDB write batch only after source and target EOD, and roll
  the pending rows back on source errors, target errors, or cancellation.
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
- RocksDB-backed `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, primary/UNIQUE and
  secondary-index maintenance, with statement and per-table commit atomicity.
- HBase access TDBs reached through the old executor path now build a TCB that
  emits an unsupported diagnostic instead of returning `NULL`.
- Local-lite `make local-lite` no longer builds `make_monitor` or top-level SQF
  `tools`, because the supported runtimes are embedded `sqlci` and
  `nativelite-server`, not the SQF service stack.
- Every `ContextCli` owns an `ExTransaction`, which canonically owns that
  session's `LocalLiteTxnContext`; the old process-global mutable transaction
  singleton is removed. Reset, disconnect, delete, and destruction release that
  context without touching peer sessions.
- LocalLite object ownership uses the effective identity in the current
  `ContextCli`, including after `SET SESSION AUTHORIZATION`; it does not use an
  environment variable or treat the compiler-session copy as authoritative.
- `nativelite-server` exclusively opens the configured store for its process
  lifetime, creates one CLI context per connection, and accepts numeric loopback
  TCP or an owner-only Unix socket. It reaps completed connection threads,
  refuses to replace non-socket, foreign, or active Unix paths, and removes
  only its own bound socket inode. Network connections, compiler requests, and
  executor requests are concurrent across session-owned connection threads;
  DDL/catalog and SQLCI utility paths use narrow locks.
- The server implements a bounded Trafodion Type 4 association and SQL dialogue
  on one listener, without DCS or ZooKeeper. The repository T4 JDBC driver
  validates connect/disconnect, direct and prepared execution, fetch, typed
  rows, autocommit/commit/rollback, internal STOPSRVR cancellation, and catalog,
  schema, table, column, and primary-key metadata.
  Session diagnostics use unnamed temporary streams and disappear even after
  an unclean process stop.

Known limits:

- `sqlci` and `nativelite-server` are the only runtime targets intended to work
  standalone today.
- Other built binaries such as `tdm_arkcmp`, `shell`, `monitor`, `trafns`,
  `sqwatchdog`, `pstartd`, and `tm` may be present in the local-lite build, but
  they are not a supported standalone database environment.
- Local-lite `sqlci` still links internal Trafodion shared libraries and a small
  Seabed baseline. Monitor-specific paths are skipped or guarded, but Seabed
  libraries remain transitive dependencies.
- The server protocol is a reduced T4 compatibility surface, not a replacement
  for DCS. Results are buffered; LOB, callable statements, batch/rowset input,
  broad metadata APIs, password, TLS, and remote deployment are not claimed.
  The current T4 driver's public `Statement.cancel()` does not dispatch during
  an active request; the protocol gate invokes its internal cancel path.
- The RocksDB DDL path uses the compiler DDL entry point where applicable and
  loads local tables as NATables from the local catalog. DML uses local executor
  TCBs and preserves compiler-generated physical row bytes. The supported type,
  constraint, statistics, authorization, and UDR surfaces remain deliberately
  bounded by the M4-M10 regression contracts.
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
TRAF_HOME=$(pwd)/core/sqf make -C core/sql/nskgmake TRAF_LOCAL_LITE=1 SQ_PHANDLE_VERIFIER=1 SQ_MTYPE=64 SQ_BTYPE=d SQ_MBTYPE=64d linuxdebug
```

Before a narrow clean rebuild, use the same ABI flags for clean and build:

```bash
TRAF_HOME=$(pwd)/core/sqf make -C core/sql/nskgmake TRAF_LOCAL_LITE=1 SQ_PHANDLE_VERIFIER=1 SQ_MTYPE=64 SQ_BTYPE=d SQ_MBTYPE=64d linuxdebugclean
OMPI_CXX=/usr/bin/g++ make local-lite
```

Do not mix SQL objects built with and without `SQ_PHANDLE_VERIFIER`; Seabed
process APIs have different C++ signatures in those two modes.

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
nativelite-server
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

## Running NativeLite Server

Use the same environment and matching local-lite libraries as SQLCI, then run:

```bash
$SQL_LIBS/nativelite-server --listen 127.0.0.1 --port 23400
```

The server exclusively owns `TRAF_LOCAL_STORE_DIR` until shutdown. A second
server or SQLCI process cannot open the same RocksDB store concurrently. Connect
with the repository Trafodion T4 JDBC driver:

```text
jdbc:t4jdbc://127.0.0.1:23400/:
```

For lifecycle or embedding use, pass an exact Unix socket path, for example
`--unix-socket /tmp/nativelite/server.sock --port 23400`. The socket is
created with mode `0600`. The M11 transport is deliberately trusted-local:
numeric TCP binds are restricted to loopback and authentication has no password
exchange. The current T4 JDBC acceptance gate uses TCP.
Existing non-socket, foreign, or active Unix paths are preserved, and shutdown
unlinks only the socket inode created by this server. It must not be exposed as
a remote production listener.

Each accepted connection receives a distinct `ContextCli` and
`LocalLiteTxnContext`. Connection threads may overlap, but requests enter a
single engine queue because the embedded CLI/compiler has process-global state.
That still permits independent, overlapping transactions: pending writes and
snapshots stay in each session context while peer requests are interleaved.
Graceful `SIGTERM` stops new accepts, closes clients, rolls back their pending
state, and closes the store.

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
- RocksDB local tables with compiler/executor-backed SELECT/INSERT/UPDATE/
  DELETE/MERGE/UPSERT, transactions, joins, aggregates, ordering, cursors,
  windows, and local scratch behavior.
- Catalog DDL for tables, views, schemas, sequences, synonyms, indexes,
  constraints, ALTER/TRUNCATE, plus local SHOWDDL/SHOWSTATS and catalog
  enumeration.
- Local users, roles and object privileges, and the bounded native/Java UDR
  adapters described by M8/M9.
- Basic scalar expressions, arithmetic, and string operations.
- Direct `DATE`, `TIME`, and `TIMESTAMP` result rendering for standalone
  expressions and local table columns.
- `exit;` and `quit;`.

Unsupported behavior:

- Cross-process concurrent writers, distributed transactions, node-level HA,
  zero-downtime upgrade, and in-place conversion of old per-table stores. M13
  accepts only the unified TransactionDB key space. M12 provides
  crash-recoverable multi-table DML, synchronous WAL policy, checkpoint,
  backup/restore, integrity verification, metrics, and
  disk-watermark gates for its declared single-node boundary.
- RI CASCADE, computed system columns, unrestricted histogram/physical metadata,
  LOB/ARRAY storage, and the full UDR server/host-rowset surface.
- HBase-backed metadata/storage operations.
- HDFS, HBase, Hive, ORC, bulk load/unload, and LOB storage access.
- Full T4/DCS compatibility, ODBC certification, REST, TrafCI, TLS, password
  authentication, and remote client connectivity. M11 supports only its reduced
  local T4 surface validated with the repository JDBC driver.
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
- `TEST005`: portable expression and query-shape coverage adapted from legacy
  core/executor regressions, including CASE, TRIM/concatenation, LIKE/OR,
  typed and untyped NULL behavior, left-join null instantiation, HAVING, and a
  scalar aggregate subquery.
- `TEST006`: portable `UNION ALL`/`UNION DISTINCT`, aggregate and joined
  derived tables, and correlated `EXISTS`/`NOT EXISTS`/scalar aggregate
  subqueries adapted from legacy core/executor regressions.
- `TEST007`: deterministic binder/runtime diagnostics for disabled set
  operations, incompatible `UNION` operands, INSERT degree and NOT NULL
  violations, scalar-subquery cardinality, and post-error session/data recovery.
- `TEST008`: aggregate/subquery binder diagnostics and executor value-evaluation
  diagnostics for invalid character conversion, numeric narrowing overflow, and
  division by zero, including successful post-error table access.
- `TEST009`: compiler-routed local DDL diagnostics, supported DML coexistence,
  rejected native external-table/service-only DDL, and post-error base-table
  verification.
- `TEST010`: `INSERT ... SELECT` with target-column reordering, source
  expressions/predicates, a `UNION ALL` source, explicit transaction rollback,
  autocommit rollback after a late primary-key conflict, and successful
  post-error transaction cleanup.
- `TEST011`: repeated prepared SELECT and INSERT execution, fresh snapshots and
  result reuse, SQLCI parameter rebinding, prepared-statement name replacement,
  and successful execution after a prepared INSERT duplicate-key error.
- `TEST012`: base-table and derived-table `NATURAL JOIN`, single- and
  multi-column `SELECT DISTINCT`, ordered `FIRST`, legacy `[ANY n]`-style
  `LIMIT`, and correlated `FIRST` over local table scans.
- `TEST013`: ASCII string functions over local `VARCHAR` rows, positive and
  negated POSIX `REGEXP` predicates with NULL input, invalid-pattern diagnostic
  `8452`, and successful table access after the executor error.
- `TEST014`: grouped and scalar `GROUP_CONCAT`, ordered and `DISTINCT` values,
  default and custom separators, an independent order key, NULL elimination,
  empty-string preservation, and all-NULL/empty-input results.
- `TEST015`: `COALESCE`, `DECODE`, `ISNULL`, `NULLIF`, and `NVL` over
  stored nullable ASCII values, including empty strings, NULL-to-NULL
  `DECODE` matching, missing defaults, and predicate use.
- `TEST016`: single-byte `ASCII` and `CHAR` over stored values, empty
  strings and NULL, ISO88591 boundary round trips, scalar subqueries,
  predicates, out-of-range diagnostic `8428`, and post-error recovery.
- `TEST017`: `DATEFORMAT`, `DAYNAME`, and `MONTHNAME` over persisted DATE
  values, including all three format styles, leap-day and year-boundary
  values, NULL propagation, `INSERT ... SELECT` assignment, and predicates.
- `TEST018`: single-byte `CONVERTTOHEX` over literals and stored CHAR/VARCHAR
  values, including ISO88591 byte boundaries, fixed-width padding, empty
  strings, NULL propagation, `INSERT ... SELECT` assignment, and predicates.
- `TEST019`: single-byte `CONVERTFROMHEX` round trips, persisted VARCHAR
  inputs, assignment and predicates, invalid half-byte and runtime odd-length
  diagnostics, legacy binder diagnostics `4043`/`4068`, and error recovery.
- `TEST020`: single-byte `TO_HEX`/`HEX` and `UNHEX`/`FROM_HEX` aliases over
  literals and stored VARCHAR values, including byte boundaries, empty/NULL
  propagation, `INSERT ... SELECT` assignment, and predicates.
- `TEST021`: environment-independent `CURRENT_USER`/`SESSION_USER`/`USER`
  invariants, including nonempty identity values, `INSERT VALUES` and
  `INSERT ... SELECT` assignment, concatenation, and predicates.
- `TEST022`: single-byte `SPACE` over constant and stored counts, including
  positive/zero/NULL results, both INSERT assignment paths, padded comparison,
  count-sensitive concatenation, oversized-result diagnostics, and recovery.
- `TEST023`: single-byte `CONCAT()` equivalence to `||` over literals and
  stored VARCHAR values, including empty strings, NULL propagation, embedded
  spaces, both INSERT assignment paths, nested concatenation, and predicates.
- `TEST024`: single-byte `INSERT()` string-function results over literals and
  stored VARCHAR/numeric arguments, including the legacy position/length
  matrix, append-at-end, empty/NULL values, both INSERT assignment paths,
  concatenation, and predicates.
- `TEST025`: single-byte `REPEAT()` over literal and stored VARCHAR/count
  arguments, including zero counts, empty/NULL values, trailing spaces, both
  INSERT assignment paths, concatenation, predicates, count diagnostics, and
  post-error recovery.
- `TEST026`-`TEST029`: UPDATE, DELETE, UPSERT, and MERGE semantics, including
  key changes, explicit transactions, statement atomicity, and diagnostics.
- `TEST030`-`TEST035`: primary/UNIQUE and secondary-index maintenance, equality,
  prefix/range, ordered and index-only access, uniqueness, and rollback.
- `TEST036`-`TEST037`: catalog DDL, constraints/defaults, views, ALTER/TRUNCATE,
  generated/identity columns, sequences, and related metadata.
- `TEST038`-`TEST039`: persisted row/NULL statistics and the bounded character,
  binary, BOOLEAN, INTERVAL, and LONG VARCHAR storage surface.
- `TEST040`: cursors, windows, grouping/sorting, cancellation cleanup, and local
  scratch-file lifecycle in the single-process executor.
- `TEST041`-`TEST042`: catalog authorization and the bounded native/Java UDR
  metadata/invocation adapters.
- `TEST043`: schema/catalog enumeration, `USE`/`SHOW SCHEMAS`, and synthetic
  `_MD_` table visibility.

Run all cases or a selected subset from the repository root:

```bash
make local-lite-regress
make local-lite-regress LOCAL_LITE_REGR_TESTS="001 003"
```

The broad legacy `core`, `executor`, and `seabase` suites are not claimed as
compatible. They still contain physical HBase/Hive behavior, shell-driven
multi-session/service-stack operations, unsupported type/collation paths, and
generated regress-tool dependencies that are absent from this checkout.
Portable SQL should be promoted only through the reviewed adapter/manifest.

The current successful set-operation surface is `UNION ALL` and `UNION`
distinct. `INTERSECT` and `EXCEPT` remain disabled by the existing binder
default and are not claimed by the local-lite lane. `TEST007` locks this
boundary to the existing `3022` diagnostic.

### Legacy Core/Executor Compatibility Audit

The audit compared the portable portions of legacy `core/TEST001`,
`core/TEST002`, `executor/TEST001`, and `executor/TEST002`, plus the ASCII
string-function subset of `charsets/TEST313`, with native `TEST001` through
`TEST025`. The legacy files remain useful as SQL-shape input, but their
environment setup, object inventory, and EXPECTED output cannot be run
unchanged in local-lite.

| Legacy area | Native status | Remaining work |
| --- | --- | --- |
| Basic table DDL, `INSERT VALUES`, scans, expressions, joins, aggregates, derived tables, subqueries, and `UNION` | Covered by `TEST001`-`TEST006` | Continue migrating only deterministic, service-independent variants. |
| Binder, executor-value, DDL, and unsupported-surface diagnostics | Covered by `TEST007`-`TEST009` | Add narrow cases only when they protect a local-lite invariant. |
| `INSERT ... SELECT` | Covered by `TEST010` | Tuple-flow autocommit is atomic within one target table; M12 adds durable journal coordination for explicit transactions that publish multiple tables. |
| SQLCI `PREPARE`/`EXECUTE` lifecycle | Covered by `TEST011`, including repeated SELECT/INSERT, new parameter values, name replacement, fresh SELECT results, and post-error recovery | Add cases only when they protect another prepared-execution invariant. |
| `NATURAL JOIN`, general `SELECT DISTINCT`, and `FIRST`/limit shapes | Covered by `TEST012`, including correlated `FIRST` through ProbeCache | `LIMIT` intentionally retains Trafodion's legacy `[ANY n]` semantics rather than ordered `FIRST` semantics. |
| POSIX `REGEXP` and additional ASCII string functions | Covered by `TEST013`, including NULL propagation, invalid-pattern `8452`, and post-error recovery | UCS2/UTF8 conversion, collation, and large-length character boundaries remain outside this portable increment. |
| `GROUP_CONCAT` ordering, `DISTINCT`, separator, and NULL forms | Covered by `TEST014`, including an order key independent of the concatenated value and empty-string separator placement | `MAX LENGTH`, overflow warning `8402`, distributed staging, and multiple incompatible aggregate orderings remain outside this portable increment. |
| `COALESCE`, `DECODE`, `ISNULL`, `NULLIF`, and `NVL` | Covered by `TEST015`, including stored NULL/empty inputs, NULL-to-NULL `DECODE` matching, missing defaults, and predicates | Charset conversion and collation combinations remain outside this portable increment. |
| `ASCII` and `CHAR` code conversion | Covered by `TEST016`, including stored NULL/empty values, ISO88591 `0..255` round trips, scalar subqueries, predicates, and diagnostic `8428` | Explicit UTF8/UCS2 forms and charset translation remain outside this portable increment. |
| `DATEFORMAT`, `DAYNAME`, and `MONTHNAME` | Covered by `TEST017`, including `DEFAULT`/`USA`/`EUROPEAN` formats, leap-day and year-boundary values, NULL propagation, assignment, and predicates | Existing compiler/executor behavior was sufficient; locale-dependent variants remain outside this portable increment. |
| `CONVERTTOHEX` over single-byte values | Covered by `TEST018`, including uppercase output, ISO88591 `00`/`7F`/`80`/`FF` boundaries, CHAR padding, VARCHAR length, empty/NULL inputs, assignment, and predicates | UTF8/UCS2 encoding and translation matrices remain outside this portable increment. |
| `CONVERTFROMHEX` over single-byte values | Covered by `TEST019`, including byte-boundary round trips, persisted empty/NULL values, assignment, predicates, invalid half-byte and runtime odd-length diagnostic `8428`, binder diagnostics `4043`/`4068`, and post-error recovery | The executor now validates both input half-bytes independently; UTF8/UCS2 conversion remains outside this portable increment. |
| `TO_HEX`/`HEX` and `UNHEX`/`FROM_HEX` aliases | Covered by `TEST020`, including ISO88591 byte boundaries, persisted empty/NULL values, assignment, and predicates | Binary/VARBINARY and UTF8/UCS2 forms remain outside this portable increment. |
| `CURRENT_USER`, `SESSION_USER`, and `USER` identity expressions | Covered by `TEST021`, including environment-independent equality/nonempty invariants, both INSERT assignment shapes, concatenation, and predicates | Existing parser/compiler/executor behavior was sufficient; configured identity text and definer-rights identity changes remain outside this portable increment. |
| Single-byte `SPACE` expressions | Covered by `TEST022`, including constant and stored counts, positive/zero/NULL results, both INSERT assignment paths, padded comparison, exact-length and concatenation predicates, diagnostics `4129`/`4062`, and post-error recovery | Existing parser/compiler/executor behavior was sufficient; explicit UTF8/UCS2 variants remain outside this portable increment. |
| Single-byte `CONCAT()` function | Covered by `TEST023`, including literal and stored VARCHAR operands, equivalence to `||`, empty/NULL inputs, embedded-space byte preservation, both INSERT assignment paths, nested use, and predicates | Existing parser/compiler/executor behavior was sufficient; UTF8/UCS2 variants remain outside this portable increment. |
| Single-byte `INSERT()` string function | Covered by `TEST024`, including literals, stored VARCHAR and numeric arguments, the legacy position/length matrix, append-at-end, empty/NULL values, both INSERT assignment paths, concatenation, and predicates | Existing binder rewrite and executor behavior were sufficient; invalid-position/negative-length diagnostics and UTF8/UCS2 variants remain outside this portable increment. |
| Single-byte `REPEAT()` function | Covered by `TEST025`, including literal and stored VARCHAR/count arguments, zero counts, empty/NULL values, trailing-space byte preservation, both INSERT assignment paths, concatenation, predicates, diagnostics `8432`/`4129`/`4116`, and post-error recovery | Existing binder/executor behavior was sufficient; unconstrained dynamic counts retain a maximum-width result type, and UTF8/UCS2 variants remain outside this portable increment. |
| Single-byte `TRIM`/`LTRIM`/`RTRIM` family | Stored `TRIM` projections are exercised by `TEST005`/`TEST013`, while `LTRIM`/`RTRIM` remain literal-only in `TEST013`; family-wide assignment and predicate coverage is not yet claimed | This is the next portable gap from `core/TEST038` and `charsets/TEST313`; cover leading/trailing/both and explicit trim-character forms, CHAR versus VARCHAR results, empty/NULL values, both INSERT assignment paths, concatenation, and predicates without adding UTF8/UCS2 variants. |
| UPDATE/DELETE/MERGE/UPSERT, views/indexes/schema objects, character/data types, statistics, authorization, and bounded UDR | Covered for the declared RocksDB-only surface by `TEST026`-`TEST043` and the eleven-case legacy allowlist | Legacy cases that require physical HBase/Hive behavior, shell-driven multi-session execution, broader collation/LOB paths, or compiler-crashing SQL remain blocked, unsafe, or excluded. |

The next primary milestone is M14 TPC-C qualification. Portable
`TRIM`/`LTRIM`/`RTRIM` compatibility remains a parallel backlog rather than the
critical path. M14 first establishes a deterministic one-warehouse schema and
loader, all five transaction profiles over T4 JDBC, Level 3 isolation evidence,
and true concurrent execution; it then scales to a reproducible multi-warehouse
TPC-C-like workload. Service drain/upgrade orchestration, active-layout backup
controls, journal consolidation, and security hardening remain parallel
productization work.

The broader effort to run portable sections from the legacy regress suites is
tracked separately in `plan/local-lite-legacy-regress-roadmap.md`. Milestone 0
provides a versioned TEST manifest, safety audit, isolated RocksDB adapter, and
baseline evidence before mutable DML and catalog surface expansion begins.

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
for the supported v1 local table types. For an insert-select plan, the normal
`ExTupleFlowTcb` feeds source ATPs to this child. In local-lite, a tuple flow
whose target identifies itself as a local insert creates an implicit
`LocalLiteTxn` when there is no explicit transaction, commits after complete
source/target EOD, and rolls back after child errors or cancellation.

### SQLCI Handler

`core/sql/sqlci/SqlCmd.cpp` calls
`LocalLiteSqlTable_process()` before CLI prepare in local-lite builds. The
handler supplies local session/catalog compatibility (`SET`/`USE`,
`GET`/`SHOW`, SHOWDDL/SHOWSTATS), schema/synonym/statistics operations,
authorization checks, and bounded UDR handling. Native HBase/Hive table DDL and
service-stack-only operations still receive explicit unsupported diagnostics.

Unqualified table names default to `TRAFODION.SEABASE.<name>`. Unquoted
identifiers are uppercased; quoted identifiers preserve case.

Normal table SELECT and DML intentionally use the compiler/executor paths
described above; the handler must not become a second general SQL executor.

All supported query execution must use executor TCBs. The SQLCI local-lite hook
must not scan RocksDB rows, insert RocksDB rows, or materialize general query
results directly.

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

Last updated after completing the bounded M11 sessionized runtime, standalone
server, and Trafodion Type 4 client-protocol acceptance gates.

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
- The same concurrency probe now covers simultaneous primary-key and UNIQUE-key
  conflicts. Exactly one writer may publish each key, duplicate writers cannot
  overwrite the persisted row, and failed UNIQUE attempts do not advance the
  keyless row-id metadata.
- Cross-process shared-store boundary enforcement for `TRAF_LOCAL_STORE_DIR`:
  a second process that tries to open an already-held local store receives an
  explicit local-lite diagnostic instead of a raw RocksDB `LOCK` message.
- M11 sessionization: mutable transaction state lives under each CLI session's
  `ExTransaction`, while storage handles remain process-owned and shared.
- M11 standalone service and reduced Trafodion Type 4 endpoint, with repository
  T4 JDBC coverage for multi-client isolation, disconnect/cancel cleanup,
  prepared execution, typed fetch, metadata, and restart persistence.
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
- Native `TEST005` moves portable coverage from `core/TEST001`, `core/TEST038`,
  and `executor/TEST002` into the isolated lane. It validates CASE and string
  expressions, LIKE/OR predicates, legacy untyped-NULL diagnostics, typed NULL
  propagation, left-join null instantiation, grouped HAVING, and a scalar
  aggregate subquery through normal executor plans.
- Native `TEST006` moves compatible `UNION ALL`/`UNION`, derived-table, and
  correlated-subquery coverage from `executor/TEST002` and `core/TEST002` into
  the isolated lane. It validates duplicate preservation and elimination,
  aggregation over a union-derived table, joins between aggregate derived
  tables, correlated `EXISTS`/`NOT EXISTS`, and correlated scalar `MAX`/`AVG`
  subqueries through normal executor plans.
- Native `TEST007` moves a first deterministic diagnostic set into the isolated
  lane. It validates disabled `INTERSECT`/`EXCEPT` (`3022`), unequal and
  incomparable `UNION` operands (`4066`/`4055`), INSERT degree and NOT NULL
  violations (`4023`/`4122`), scalar-subquery cardinality (`8401`), and confirms
  that failures neither poison the SQLCI session nor partially insert rows.
- Native `TEST008` validates multi-column subquery renaming (`4477`),
  nongrouping columns (`4021`), misplaced aggregates (`4015`), mixed aggregate
  scopes (`4006`), invalid character conversion (`8413`), numeric narrowing
  overflow (`8411`), and division by zero (`8419`). A final ordered table scan
  confirms that the executor errors leave both the session and stored rows
  usable.
- Native `TEST009` validates compiler-routed duplicate CREATE, supported
  CHECK/DEFAULT definitions, rejected BLOB definitions, DROP CASCADE, and
  missing-table diagnostics. It also exercises supported DML, index/view/
  sequence/schema/synonym DDL, ALTER/TRUNCATE, and the explicit rejection of
  UPSERT USING LOAD and native external-table DDL.
- Native `TEST010` migrates the basic `INSERT ... SELECT` shapes from legacy
  executor tests. It covers column reordering, expressions, source filtering,
  a `UNION ALL` source, explicit rollback, and a late primary-key conflict that
  must not partially publish earlier tuple-flow rows. A subsequent successful
  insert confirms that error cleanup releases the implicit local transaction.
- Native `TEST011` migrates the prepared SELECT/INSERT lifecycle from legacy
  executor/core tests. It covers repeated SELECT execution against fresh local
  snapshots, SQLCI parameter rebinding for SELECT and INSERT, prepared-statement
  name replacement, and deterministic recovery after a duplicate-key error.
- Native `TEST012` migrates deterministic `NATURAL JOIN`, general
  `SELECT DISTINCT`, and `FIRST`/limit shapes from legacy executor/core tests.
  Its correlated `FIRST` case also guards scan code generation: columns supplied
  as characteristic inputs must not be appended to the inner scan's physical
  fetched-column list and remapped to the inner row ATP.
- Native `TEST013` migrates POSIX `REGEXP` predicates from legacy
  `executor/TEST002` and portable ASCII string functions from
  `charsets/TEST313`. It covers stored nullable `VARCHAR` inputs for
  `UPPER`/`LOWER`, substring/position/replacement, left/right/padding, plus
  standalone repeat, length, insert, trim, and space expressions. An invalid
  pattern locks diagnostic `8452`, followed by a successful table read.
- Native `TEST014` migrates portable `GROUP_CONCAT` shapes from legacy
  `executor/TEST002`. It covers ascending and descending order, duplicate
  preservation and `DISTINCT`, default and custom separators, grouped and
  scalar aggregates, NULL elimination, empty-string preservation, and
  all-NULL/empty inputs. The executor aggregate now initializes to NULL, skips
  NULL inputs without erasing an existing result, and tracks a zero-length
  input as an accumulated value for separator placement, while group-by keeps
  independent aggregate order keys available to its child plan.
- Native `TEST015` migrates portable ASCII/NULL conditional expressions from
  `charsets/TEST313`. It covers `COALESCE`, `DECODE`, `ISNULL`, `NULLIF`,
  and `NVL` over stored nullable values in projections and predicates,
  including empty strings, all-NULL inputs, NULL-to-NULL `DECODE` matching,
  and a missing `DECODE` default. Existing compiler/executor behavior required
  no implementation change.
- Native `TEST016` migrates portable single-byte `ASCII`/`CHAR` expressions
  from `charsets/TEST313`. It covers stored nullable values, the empty-string
  code `0`, ISO88591 `0..255` round trips, an explicit ISO88591 result,
  scalar subqueries, predicates, out-of-range diagnostic `8428`, and
  post-error recovery. Existing compiler/executor behavior required no
  implementation change.
- Native `TEST017` migrates portable `DATEFORMAT`/`DAYNAME`/`MONTHNAME`
  expressions from `charsets/TEST313`. It covers all three deterministic
  DATEFORMAT styles, persisted DATE values at leap-day and year boundaries,
  NULL propagation, `INSERT ... SELECT` assignment, and predicate use.
  Existing compiler/executor behavior required no implementation change.
- Native `TEST018` migrates portable single-byte `CONVERTTOHEX` expressions
  from `charsets/TEST313`. It covers uppercase output, ISO88591 byte
  boundaries, fixed CHAR padding versus VARCHAR length, empty and NULL inputs,
  `INSERT ... SELECT` assignment, and predicate use. Existing
  compiler/executor behavior required no implementation change.
- Native `TEST019` adds portable single-byte `CONVERTFROMHEX` round trips and
  migrates operand-type and odd declared-length diagnostics from
  `compGeneral/TEST006`. It covers persisted empty/NULL values, assignment,
  predicates, invalid half-bytes, runtime odd length, binder diagnostics
  `4043`/`4068`, and post-error recovery. The executor now groups each
  half-byte validity test independently so an invalid second character cannot
  pass merely because the first character is numeric.
- Native `TEST020` migrates portable single-byte `TO_HEX` expressions from
  `seabase/TEST004` and pairs them with the `HEX`, `UNHEX`, and `FROM_HEX`
  aliases. It covers ISO88591 byte boundaries, persisted text, trailing space,
  empty and NULL values, `INSERT ... SELECT` assignment, and predicate use.
  Existing parser/compiler/executor behavior required no implementation
  change.
- Native `TEST021` migrates portable `CURRENT_USER`, `SESSION_USER`, and `USER`
  assignment, comparison, and concatenation shapes from `charsets/TEST313`.
  Its EXPECTED output asserts equality and nonempty invariants instead of the
  configured identity text, and covers both INSERT assignment paths plus
  predicates over persisted values. Existing parser/compiler/executor behavior
  required no implementation change.
- Native `TEST022` expands portable single-byte `SPACE` coverage from the
  standalone expression in `TEST013` to constant and stored counts,
  positive/zero/NULL results, both INSERT assignment paths, SQL padded
  comparison semantics, count-sensitive concatenation, diagnostics
  `4129`/`4062`, and post-error recovery. Existing parser/compiler/executor
  behavior required no implementation change.
- Native `TEST023` migrates portable single-byte `CONCAT()` assignment,
  comparison, and nested concatenation shapes from `charsets/TEST313`. It
  covers literal and stored VARCHAR operands, exact equivalence to `||`, empty
  strings, NULL propagation, embedded-space byte preservation, both INSERT
  assignment paths, and predicates. Existing parser/compiler/executor behavior
  required no implementation change.
- Native `TEST024` expands portable single-byte `INSERT()` string-function
  coverage from the standalone literal in `TEST013` using shapes from
  `charsets/TEST312` and `charsets/TEST313`. It covers literal and stored
  VARCHAR/numeric arguments, the legacy position/length matrix, append-at-end,
  empty strings, NULL propagation across all four arguments, both INSERT
  assignment paths, concatenation, and predicates. Existing binder rewrite and
  executor behavior required no implementation change.
- Native `TEST025` expands portable single-byte `REPEAT()` coverage from the
  standalone literal in `TEST013` using stored-operand and diagnostic shapes
  from `core/TEST038`. It covers literal and stored VARCHAR/count arguments,
  zero counts, empty strings, NULL propagation, trailing-space byte
  preservation, both INSERT assignment paths, concatenation, predicates,
  diagnostics `8432`/`4129`/`4116`, and post-error recovery. Existing
  binder/executor behavior required no implementation change.
- Autocommit tuple-flow inserts now use `LocalLiteTxnManager` as an implicit
  statement transaction when no explicit local transaction is active. The
  tuple flow commits only after complete source/target EOD and rolls back on
  either child error or cancellation.
- RocksDB SQLCI smoke now verifies that an autocommit multi-row `INSERT VALUES`
  with a late primary-key conflict does not partially publish an earlier row.

The transaction/concurrency roadmap Phases 1 through 5 are complete for the
documented local-lite v1 boundary. M12 subsequently adds the selected
single-keyspace transaction contract and journal-coordinated, crash-recoverable
multi-table DML publication. M13 makes the TransactionDB layout the exclusive
catalog/table runtime format. Cross-process writers,
TMF coordination, distributed execution, node HA, and zero-downtime upgrade
orchestration remain later work.

Remaining, in implementation order:

1. Complete **M14 TPC-C qualification**: deterministic schema/load, all five
   transactions through T4 JDBC, consistency and Level 3 isolation tests,
   concurrent compiler/executor work, then multi-warehouse performance and
   recovery evidence.
2. In parallel, productize the M13 boundary with service drain/upgrade,
   active-layout backup controls, recovery-journal consolidation, and security
   hardening.
3. Continue portable SQL compatibility increments, beginning with the
   single-byte `TRIM`/`LTRIM`/`RTRIM` family.

The authoritative milestone definitions and completion gates are in
`plan/local-lite-legacy-regress-roadmap.md`.

- [ ] **Complete M14 TPC-C qualification for the supported single-node
  trusted-local boundary.**
  - [x] Pin TPC-C 5.11.0, document dialect deviations, and version the schema,
    driver, workload, terminal, retry, and reporting contracts.
  - [x] Add deterministic one-warehouse creation/loading, exact cardinality
    checks, consistency queries, interrupted-load recovery, and restore proof.
  - [x] Implement New-Order, Payment, Order-Status, Delivery, and Stock-Level
    through prepared statements on the real reduced T4 JDBC endpoint.
  - [x] Prove the required Level 3 isolation boundary, including predicate and
    phantom conflicts, every required isolation test, timeout/deadlock policy,
    and exactly-once effects across bounded retries.
  - [x] Remove global compiler/executor request serialization while preserving
    session ownership, DDL/catalog safety, cancellation, and peer survival.
  - [ ] Add multi-warehouse workload mix, latency/throughput/resource metrics,
    crash/backup/restore-under-load evidence, repetitions, and variance gates.
  - [ ] Add `make local-lite-m14`; publish separate functional, TPC-C-like, and
    formal-compliance checklists. Do not label results `tpmC` before the formal
    specification and disclosure boundary is satisfied.

- [x] **Complete M13 exclusive unified storage for restart-based
  single-process activation.**
  - [x] Create fresh stores directly in format-version-2 `transactiondb/`.
  - [x] Atomically publish versioned format, activation, and layout markers.
  - [x] Reject incomplete/unsupported markers and any old `catalog/` or
    `data/` layout before opening SQL traffic.
  - [x] Remove old-layout migration, export, rollback, and runtime fallback.
  - [x] Prove interruption/retry, old-layout rejection, unified-only
    DDL/DML/drop, and restart persistence.
  - [x] `make local-lite-m13` composes M12 and the M13 SQLCI format gate.

- [x] **Complete M12 transactional storage and recovery for the declared
  single-node boundary.**
  - [x] Define backend-neutral engine, session, transaction, status, metrics,
    and bounded streaming cursor contracts.
  - [x] Put catalog, base, UNIQUE, and secondary-index records through the
    common single-keyspace atomicity/snapshot contract.
  - [x] Migrate legacy fixed-buffer metadata keys to collision-free version-2
    encodings with exact-count and collision checks.
  - [x] Run RocksDB TransactionDB and SQLite WAL through the same correctness,
    crash-recovery, fault, and workload gates; select TransactionDB from those
    results rather than peak throughput alone.
  - [x] Add synchronous durability, checkpoint, backup/restore, integrity,
    metrics, and disk-watermark support.
  - [x] Protect live-layout multi-table DML with preflight, a durable commit
    journal, idempotent table markers, and startup replay; verify it through a
    real SQLCI interruption/restart test.
  - [x] `make local-lite-m12` composes the M12A, M12B, and M12C gates.

- [x] **Complete M11 sessionized runtime and standalone server.**
  - [x] Each `ContextCli` owns an `ExTransaction`, which creates, resets, and
    destroys its canonical `LocalLiteTxnContext`.
  - [x] Transaction TCBs, tuple flow, scan/DML TCBs, DDL, and executor-root
    snapshots receive the active context explicitly; the process-global
    `LocalLiteTxnState` singleton is removed.
  - [x] `make local-lite-m11a` covers independent context isolation,
    commit/rollback, same-key conflicts, and reset/destroy cleanup at the
    local-store transaction boundary.
  - [x] A SQLCI probe constructs two complete `ContextCli` instances, keeps
    overlapping transactions active, and validates isolation, independent
    completion, reset/delete rollback, and peer survival.
  - [x] LocalLite DDL ownership resolves from the authoritative current
    `ContextCli`; `TEST041` verifies ownership, GRANT, and identity switching,
    while `executor/TEST014` verifies CTAS remains valid inside the bounded
    explicit-transaction contract.
  - [x] `nativelite-server` owns the store, creates one CLI context per client,
    supports loopback TCP and protected 0600 Unix sockets, reaps completed
    connection threads, and shuts down cleanly without deleting a replaced
    socket pathname.
  - [x] The repository Trafodion T4 JDBC driver covers association and dialogue
    startup, prepared-statement reuse, typed rows, overlapping transactions,
    disconnect rollback, internal STOPSRVR cancellation with an active peer,
    bounded DatabaseMetaData calls, store exclusivity, and restart persistence.
  - [x] `make local-lite-m11` composes the M11A, M11B, and M11C gates.

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

- [x] **Route supported local DML and index operations through the bounded
  RocksDB path.**
  - `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, and `CREATE INDEX` are implemented
    for the declared local surface and covered by native `TEST026`-`TEST035`.
    Service-stack-only variants retain explicit unsupported diagnostics.

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
  - Executor `INSERT INTO ... VALUES` and `INSERT ... SELECT` now evaluate the
    compiler-generated insert expression and persist the complete table row as
    `LLBR1` binary aligned row data through `LocalLiteRocksDBStore`.
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
    plus `INSERT ... SELECT`, followed by projected executor scans.

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
make local-lite-m11
```

## Design Rules

- Keep the full Trafodion build unchanged unless `TRAF_LOCAL_LITE=1` is set.
- Keep all local-lite behavior compile-time gated by `TRAF_LOCAL_LITE`.
- Prefer small native stubs over compiling Java/Hadoop-backed code.
- Keep the RocksDB local store native-only and process-owned; client sessions
  must reach it through their explicit transaction context and the serialized
  server engine boundary.
- Disabled HDFS/Hive/HBase paths must fail explicitly.
- Treat `sqlci` and the bounded M11 `nativelite-server` as the supported
  standalone runtimes; do not imply `mxosrvr`, DCS, REST, or the full SQF
  service stack is standalone.
