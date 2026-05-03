# Trafodion Local-Lite Runtime

## What It Is

Local-lite is a native-only, single-process build of the Trafodion SQL compiler and executor. It allows you to run `sqlci` directly — no shell scripts, no monitor process, no Java, no Hadoop/HBase/ZooKeeper cluster.

## What It Is Not

Local-lite is not a production deployment. It is a development and testing tool. There is no transaction management (TM/DTM), no JDBC/ODBC connectivity, no HBase storage engine, no distributed query execution. It runs queries entirely in-process on a single node.

## Quick Start

```bash
# Clone and build
git clone https://github.com/apache/trafodion.git
cd trafodion
TRAF_HOME=$(pwd)/core/sqf make -C core/sql/nskgmake TRAF_LOCAL_LITE=1 SQ_MTYPE=64 SQ_BTYPE=d SQ_MBTYPE=64d linuxdebug

# Run
SQL_LIBS=$(pwd)/core/sql/lib/linux/64bit/debug
SQF_LIBS=$(pwd)/core/sqf/export/lib64d
LD_LIBRARY_PATH=$SQL_LIBS:$SQF_LIBS $SQL_LIBS/sqlci
```

No scripts. No environment setup. Just point `LD_LIBRARY_PATH` at the two library directories and run the binary.

## What Works

- SQL parsing, binding, normalization
- Query optimization (single-node plans)
- Expression evaluation
- `SELECT` queries against `VALUES` clauses
- Basic scalar expressions, arithmetic, string operations
- `SHOW`/`DESCRIBE` metadata commands (limited)
- `exit;` / `quit;`

## What Doesn't Work

- DDL statements (CREATE TABLE, INSERT, UPDATE, DELETE) — require HBase storage
- HDFS access — not linked
- HBase access — not linked
- Hive access — not linked
- Java/JDBC/ODBC — not linked
- Distributed query execution — single node only
- Transaction management — TM/DTM not started
- `sqstart`/`sqstop`/`sqgen`/`sqshell` scripts — irrelevant for local-lite

## Switching Between Modes

Local-lite is a **compile-time mode**. To switch:

- **Local-lite**: build with `TRAF_LOCAL_LITE=1`
- **Full Trafodion**: build without `TRAF_LOCAL_LITE=1` (i.e., standard `make all`)

The same source tree supports both. All local-lite changes are gated behind `#ifdef TRAF_LOCAL_LITE` / `#ifndef TRAF_LOCAL_LITE` preprocessor guards. The full build path is never altered.

## Runtime Dependencies

sqlci links against:
- ~25 Trafodion internal shared libraries (libcommon, libexecutor, liboptimizer, etc.)
- 9 Seabed shared libraries (libsm, libsbms, libsbfs, libsbutil, libwin, libstfs, libstmlib, libsqstatesb, libsqauth) — these are transtitive dependencies providing foundational I/O primitives
- Standard system libraries (libc, libstdc++, libcurl, libssl, libcrypto, etc.)

Zero Java, zero Hadoop, zero HBase, zero ZK dependencies. No monitor process is spawned. No config files are required (the `ms.env` probe is harmless).
