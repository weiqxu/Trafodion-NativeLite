# Trafodion Local-Lite

## Overview

`local-lite` is a native-only Trafodion build and runtime mode for local SQL
engine development. It removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS,
REST, TrafCI, and Java client modules from the selected build path.

The current useful runtime target is `sqlci` in local-lite mode. It can start as
a single local process and execute SQL that stays inside the compiler/executor,
for example `SELECT` statements over `VALUES` clauses.

Local-lite is not a complete standalone database. It does not include a local
storage engine. DDL, INSERT/UPDATE/DELETE, HBase-backed tables, HDFS, Hive,
distributed execution, JDBC/ODBC connectivity, and transaction-service behavior
remain outside the supported runtime surface.

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

Known limits:

- `sqlci` is the only runtime target intended to work standalone today.
- Other built binaries such as `tdm_arkcmp`, `shell`, `monitor`, `trafns`,
  `sqwatchdog`, `pstartd`, and `tm` may be present in the local-lite build, but
  they are not a supported standalone database environment.
- Local-lite `sqlci` still links internal Trafodion shared libraries and a small
  Seabed baseline. Monitor-specific paths are skipped or guarded, but Seabed
  libraries remain transitive dependencies.
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
SQLite, curl, OpenSSL, readline, ncurses, bison, flex, Perl, Python, and native
development headers. It intentionally does not install Java, Maven, Hadoop,
HBase, or Hive.

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

No `sqenv.sh`, `sqgen`, `sqstart`, monitor, DCS, REST, ZooKeeper, HBase, Hadoop,
or Java process is required for this `sqlci` path.

Supported local-lite `sqlci` behavior:

- SQL parsing, binding, normalization, optimization, and executor startup.
- `SELECT` queries against `VALUES` clauses.
- Basic scalar expressions, arithmetic, and string operations.
- `exit;` and `quit;`.

Unsupported behavior:

- `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, and HBase-backed metadata/storage
  operations.
- HDFS, HBase, Hive, ORC, bulk load/unload, and LOB storage access.
- JDBC/ODBC, DCS, REST, TrafCI, and remote client connectivity.
- Distributed query execution.
- Full transaction service behavior through TM/DTM/RMS.

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

## Design Rules

- Keep the full Trafodion build unchanged unless `TRAF_LOCAL_LITE=1` is set.
- Keep all local-lite behavior compile-time gated by `TRAF_LOCAL_LITE`.
- Prefer small native stubs over compiling Java/Hadoop-backed code.
- Do not add a local storage engine under the local-lite name until that work is
  intentionally designed.
- Disabled HDFS/Hive/HBase paths must fail explicitly.
- Treat `sqlci` standalone as the first supported runtime milestone; do not
  imply `mxosrvr`, DCS, REST, or the full SQF service stack is standalone.

