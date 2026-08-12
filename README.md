# Trafodion Local-Lite

This checkout includes a `local-lite` build and runtime mode for native
Trafodion SQL engine development.

`local-lite` removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS, REST, TrafCI,
and Java client modules from the selected build path. The current supported
runtime is `sqlci` as a single-process command-line SQL engine over an embedded
RocksDB catalog and table store.

Local-lite is not a complete standalone database. Its bounded RocksDB-only
surface includes transactions, DDL/DML, indexes, metadata/statistics,
authorization, and constrained UDR execution through normal compiler/executor
paths. It does not yet provide session-isolated transaction state, atomic
multi-table commit/recovery, a standalone multi-client server, JDBC/ODBC, the
DCS/REST stack, distributed execution, or an HBase/HDFS/Hive runtime.

Full details are in:

```text
plan/local-lite.md
```

The original Apache Trafodion project overview is preserved in:

```text
plan/README.trafodion.md
```

## Current Status (verified 2026-08-12)

The bounded M1-M10 RocksDB-only, single-process scope is complete. In this
checkout, `make local-lite`, `make local-lite-m10`, and
`scripts/test-local-lite-runtime.sh` pass. M11 sessionization/server work and
M12 transactional storage/recovery are planned and have not started.

### Functional boundary

| Area | Validated local-lite surface | Not currently claimed |
| --- | --- | --- |
| DML and storage | Single-process transactions; INSERT, UPDATE, DELETE, UPSERT, and MERGE; secondary indexes and statement-level atomicity | Session-isolated transaction state, atomic multi-table commit, and crash recovery |
| Catalog and DDL | Schemas, views, synonyms, sequences, defaults, CHECK and bounded RI constraints, identity columns, triggers, and persisted basic statistics | RI CASCADE, computed system columns, full histograms, and unrestricted physical `_MD_` behavior |
| Types and executor | ISO88591/UTF8/UCS2, binary types, BOOLEAN, INTERVAL, LONG VARCHAR, cursors, window/grouping operations, local sort/scratch, and cancellation cleanup | LOB/ARRAY and broader collation support, ESP fan-out, and distributed execution |
| Authorization and UDR | Local users, roles, ownership/privileges, and bounded native/Java UDR adapters | Password/external identity services, the full UDR server, and host-rowset behavior |
| Runtime and clients | One local `sqlci` process using normal compiler/executor paths over RocksDB | A multi-client server, JDBC/ODBC, DCS/REST, and HBase/HDFS/Hive runtimes |

The detailed milestone evidence and boundaries are maintained in
[`plan/local-lite-legacy-regress-roadmap.md`](plan/local-lite-legacy-regress-roadmap.md).
The separate newregress qualification order is in
[`plan/local-lite-newregr-roadmap.md`](plan/local-lite-newregr-roadmap.md).

### Regression snapshot

| Test surface | Inventory | Current evidence |
| --- | ---: | --- |
| Native local-lite lane | 43 TEST/EXPECTED cases | **43/43 pass**, zero non-empty DIFF files |
| Unmodified Trafodion legacy allowlist | 11 cases | **11/11 pass**, zero non-empty DIFF files |
| Audited Trafodion legacy inventory | 122 unique primary TEST inputs from 9 standard suites | 11 runnable, 51 blocked, 42 unsafe, and 18 excluded |
| Remaining standard suites | 14 Hive and 25 QAT cases | Hive explicitly excluded; QAT classified as blocked/unassessed pending a shared-state adapter |
| Separate `newregr` inventory | 281 statically paired cases and 1 unpaired MVS input, plus custom performance workloads | No local-lite execution/convergence result yet |

The standard `runallsb` surface contains 161 logical cases across 11 suites:
the 122 audited inputs plus 14 Hive and 25 QAT cases. The eleven directly passing
legacy cases are therefore about 6.8% of that unchanged upstream test surface;
this is a direct compatibility measure, **not** a feature-completion percentage.
The native and legacy lanes together currently provide 54 passing test
contracts, but they are not a one-to-one mapping onto upstream tests.

The manifest contains 134 rows because mixed legacy TEST files can be split
into independently classified sections: 11 runnable, 52 blocked, 50 unsafe,
and 21 excluded rows. A 2026-08-12 re-probe of all 49 safe M2-M6 blocked rows
promoted five exact matches. After rerunning all initial timeouts with a
120-second limit, the other results were 29 output differences, 13 timeouts,
and two SQLCI aborts. The current observations are versioned
in [`reprobe-m2-m6-2026-08-12.tsv`](core/sql/regress/localLiteLegacy/reprobe-m2-m6-2026-08-12.tsv).
Unsafe entries generally require shell commands, multiple SQLCI processes,
helper binaries, Java, or the Trafodion service stack; excluded entries are
predominantly physical HBase/Hive behavior.

No current full `runallsb`, Hive, QAT, or `newregr` run is being claimed. Those
surfaces are unassessed, not failed. See the
[`localLiteLegacy` adapter README](core/sql/regress/localLiteLegacy/README.md)
for the inventory, safety rules, and probe workflow.

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

## Quick Regress Run

After building the local-lite SQL binary, run the native TEST/EXPECTED lane
without starting SQF, TMF, HBase, Hadoop, or ZooKeeper:

```bash
make local-lite-regress
```

Run selected cases with:

```bash
make local-lite-regress LOCAL_LITE_REGR_TESTS="001 003"
```

Run the bounded M10 convergence gate (all 43 native cases plus the complete
eleven-case legacy allowlist) and inspect both the adapted and complete upstream
inventories with:

```bash
make local-lite-m10
make local-lite-legacy-audit
make local-lite-regress-inventory
```

The runner prints the temporary artifact directory containing `RAWnnn`,
`LOGnnn`, and `DIFFnnn` files. The cases and baselines live under
`core/sql/regress/localLite`.

Use `make all` for the normal full Trafodion build.
