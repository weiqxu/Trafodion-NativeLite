# Local-Lite Build Design

## Goal

Create a first-stage local-only Trafodion build target that compiles and links a core C/C++ SQL/CLI subset without requiring Java, Maven, HDFS, HBase, Hive, DCS, REST, or Java client components.

This stage is a build-slimming milestone. It does not implement a local storage engine and does not promise a working `CREATE TABLE` / `INSERT` / `SELECT` database loop.

## Scope

The new build target is named `local-lite`.

The original Trafodion build targets remain unchanged. `make all`, packaging targets, Java client builds, and Hadoop-backed behavior continue to follow the existing build graph unless `local-lite` is explicitly requested.

## Keep In First Stage

- SQF base components needed by core C/C++ SQL binaries:
  - `sqevlog`
  - `seabed`
  - `trafconf`
  - `win`
  - `stfs` stub build
- Core SQL compiler, optimizer, executor, and minimum SQL/CLI binaries needed to verify compile and link.
- Existing C/C++ build infrastructure under `core`, `core/sqf`, and `core/sql/nskgmake`.

## Exclude In First Stage

- Maven and jar builds.
- Java client and service modules:
  - `jdbcT4`
  - `jdbc_type2`
  - `trafci`
  - `rest`
  - `dcs`
  - `wms`
- HBase utility and transaction components:
  - `core/sqf/hbase_utilities`
  - `core/sqf/src/seatrans/hbase-trx`
  - `core/sqf/src/seatrans/tm/hbasetmlib2`
- Runtime access to HDFS, Hive, and HBase.

## Build Entry Points

Add a parallel target chain:

- Top-level `Makefile`
  - Add `local-lite`, forwarding to `core`.
- `core/Makefile`
  - Add `local-lite`, building only native core prerequisites and the SQF/SQL local-lite subset.
  - Do not invoke `jdbc_jar`, `jdbc_type2_jar`, `dcs`, `rest`, `trafci`, `odb`, package, or installer targets.
- `core/sqf/Makefile`
  - Add `local-lite`.
  - Use a local-lite component list that excludes `hbase_utilities`.
  - Make the SQL build path independent of `hbase_utilities` when `local-lite` is selected.
- `core/sqf/src/tm/Makefile`
  - Add `local-lite` support through either a target or a `LOCAL_LITE=1` variable.
  - Skip `cp_trx_jar`, `libshbasetmlib.so`, `hbase-trx`, and `hbasetmlib2`.
  - Avoid `JAVA_HOME` include paths and `-ljvm` in local-lite builds.

## Compile-Time Switch

Introduce a single build macro:

```make
TRAF_LOCAL_LITE=1
```

Native make targets pass this into C/C++ builds as:

```make
-DTRAF_LOCAL_LITE
```

The macro is only active for `local-lite`. It must not change default builds.

## SQL Executor Strategy

In `core/sql/nskgmake/executor/Makefile`, the local-lite build should exclude JNI-heavy sources where possible:

- `JavaObjectInterface.cpp`
- `HdfsClient_JNI.cpp`
- `HiveClient_JNI.cpp`
- `HBaseClient_JNI.cpp`
- `SequenceFileReader.cpp`
- `OrcFileReader.cpp`

If dependent executor classes are still required for linking, keep their public symbols available through small C++ stubs rather than compiling Java/Hadoop-backed implementations.

Potential stub file:

```text
core/sql/executor/LocalLiteStorageStubs.cpp
```

Stub behavior:

- Compile and link cleanly.
- Return an existing error code where one exists.
- Emit or propagate a clear diagnostic equivalent to `not supported in local-lite build`.
- Never pretend that HDFS, Hive, or HBase work.

## Optimizer Strategy

The optimizer currently has direct HDFS references, including `HDFSHook.h`, which includes `hdfs.h`.

For local-lite:

- Avoid requiring Hadoop headers.
- Guard direct `hdfs.h` includes behind `TRAF_LOCAL_LITE` where needed.
- Provide local-lite stubs for HDFS statistics hooks that return unavailable or empty statistics.

Potential stub file:

```text
core/sql/optimizer/LocalLiteHdfsHookStubs.cpp
```

## Transaction Manager Strategy

The transaction manager currently links Java/HBase transaction support through `libjvm`, `hbase-trx`, and `hbasetmlib2`.

For local-lite:

- Build only the TM pieces required by the rest of the native build.
- Exclude HBase transaction library construction.
- Replace HBase RM branch operations with local-lite stubs that return unsupported when invoked.
- Do not require `JAVA_HOME`.

Potential stub file:

```text
core/sqf/src/tm/LocalLiteTmStubs.cpp
```

## Error Handling

All disabled Hadoop-backed paths must fail explicitly. Acceptable outcomes are:

- Existing Trafodion diagnostics for unsupported operations.
- A new local-lite-specific diagnostic if adding one is lower risk than reusing an existing error.
- A clear log message for startup-time disabled components.

Silent success is not acceptable for disabled storage or transaction paths.

## Verification

The first implementation is complete when:

- `make local-lite` completes.
- The build log contains no Maven invocation.
- The build does not require `JAVA_HOME`.
- The build does not enter:
  - `core/sqf/hbase_utilities`
  - `core/sqf/src/seatrans/hbase-trx`
  - `core/sqf/src/seatrans/tm/hbasetmlib2`
- Produced local-lite binaries and libraries link.
- Dynamic dependency checks on produced binaries do not show `libjvm` or `libhdfs`.
- If a minimum CLI binary can start, a smoke test confirms startup. If startup is not yet possible, verification is limited to compile, link, and dependency checks.

## Non-Goals

- Implementing a local storage engine.
- Preserving HBase/HDFS/Hive SQL behavior in local-lite.
- Packaging a distributable product.
- Removing Java/Hadoop code from the repository.
- Changing default Trafodion build behavior.

## Risks

- SQL executor and optimizer may require broader stubbing than the initial source list suggests.
- TM may have non-obvious link dependencies on HBase RM symbols.
- Some generated headers or message catalogs may assume the full build graph.
- Existing build scripts may rely on environment setup normally created by full Trafodion build tools.

## Rollout

Implement in small steps:

1. Add no-op or forwarding `local-lite` targets without changing existing targets.
2. Trim Java/Maven modules from the local-lite build graph.
3. Trim SQF HBase utility and TM HBase transaction dependencies.
4. Trim SQL executor/optimizer Java/Hadoop source dependencies.
5. Add stubs only where the linker proves they are needed.
6. Add verification commands and document the expected local-lite build result.
