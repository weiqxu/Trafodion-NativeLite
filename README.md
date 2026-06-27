# Trafodion Local-Lite

This checkout includes a `local-lite` build and runtime mode for native
Trafodion SQL engine development.

`local-lite` removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS, REST, TrafCI,
and Java client modules from the selected build path. The current supported
runtime milestone is `sqlci` as a local single-process command-line SQL engine
for compiler/executor-only SQL and minimal local RocksDB table smoke tests.

Local-lite is not a complete standalone database. It has a narrow
RocksDB-backed local table path for `sqlci` with local executor scan and insert
TCBs, but no transactions, indexes, privileges, JDBC/ODBC service path,
DCS/REST stack, or HBase/HDFS/Hive runtime.

Full details are in:

```text
plan/local-lite.md
```

The original Apache Trafodion project overview is preserved in:

```text
plan/README.trafodion.md
```

## Quick Build

Install native dependencies:

```bash
scripts/install-local-lite-deps.sh --dry-run
scripts/install-local-lite-deps.sh -y
```

Build local-lite:

```bash
OMPI_CXX=/usr/bin/g++ make local-lite
```

## Quick SQLCI Run

For the current repository build layout:

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
>>exit;
```

Minimal local table smoke example:

```sql
>>CREATE TABLE t(a INT, b VARCHAR(20));
>>INSERT INTO t VALUES (1, 'one'), (2, 'two');
>>SELECT * FROM t;
>>DROP TABLE t;
>>exit;
```

Use `make all` for the normal full Trafodion build.
