# Trafodion Local-Lite

This checkout includes a `local-lite` build and runtime mode for native
Trafodion SQL engine development.

`local-lite` removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS, REST, TrafCI,
and Java client modules from the selected build path. The current supported
runtime milestone is `sqlci` as a local single-process command-line SQL engine
for queries that do not require storage services.

Local-lite is not a complete standalone database. It has no local storage
engine, no JDBC/ODBC service path, no DCS/REST stack, and no HBase/HDFS/Hive
runtime.

Full details are in:

```text
docs/local-lite.md
```

The original Apache Trafodion project overview is preserved in:

```text
README.trafodion.md
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

Use `make all` for the normal full Trafodion build.
