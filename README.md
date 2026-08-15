# Trafodion Local-Lite

This checkout includes a `local-lite` build and runtime mode for native
Trafodion SQL engine development.

`local-lite` removes Java, Maven, Hadoop, HDFS, HBase, Hive, DCS, REST, TrafCI,
and Java client modules from the selected build path. The supported runtimes are
standalone `sqlci` and the M11 `nativelite-server`, both using the normal native
compiler/executor over an embedded RocksDB catalog and table store.

Local-lite is not a complete standalone database. Its bounded single-node
surface includes transactions, DDL/DML, indexes, metadata/statistics,
authorization, constrained UDR execution, session-owned transaction contexts,
and a reduced Trafodion Type 4 endpoint validated with the repository T4 JDBC
driver. M12 adds a backend-neutral transactional storage contract, a selected
RocksDB TransactionDB engine, durable multi-table DML publication/recovery, and
versioned metadata keys plus backup/restore tooling. M13 makes that
TransactionDB the exclusive catalog/table storage format and rejects old
per-table stores. It does
not provide password/TLS
authentication, full Trafodion wire
compatibility, the DCS/REST stack, distributed execution, or an
HBase/HDFS/Hive runtime.

Full details are in:

```text
plan/local-lite.md
```

The original Apache Trafodion project overview is preserved in:

```text
plan/README.trafodion.md
```

## Current Status (verified 2026-08-15)

The bounded M1-M13 scope is complete; M14 TPC-C qualification is planned as the
next primary milestone. Each `ContextCli` owns an `ExTransaction`,
which is now the canonical owner of that session's `LocalLiteTxnContext`;
executor/DDL paths receive it explicitly, and reset, disconnect, or destruction
discard only that session's pending writes and snapshots. The M11A gate covers
two overlapping CLI contexts, isolation, independent completion, deterministic
same-key conflict, reset/delete cleanup, snapshot release, and context reuse.
The effective authorization identity and new-object ownership also come from
the current `ContextCli`, including after `SET SESSION AUTHORIZATION`; the
compiler session is a propagated mirror rather than the LocalLite authority.

M11B adds a long-running `nativelite-server` that exclusively owns the store,
creates one CLI context per connection, accepts loopback TCP or owner-only Unix
sockets, reaps completed connection threads, and survives clean and unclean
restart with committed data intact. Unix startup preserves non-socket, foreign,
or active paths, and shutdown removes only the exact socket inode created by
the server. Per-session diagnostics use unnamed temporary streams, so an
unclean stop does not leave credential- or SQL-bearing capture files.
M11C now selects a reduced Trafodion Type 4 protocol. The same loopback listener
implements the association handshake and SQL dialogue without DCS or ZooKeeper.
Implemented operations cover connect/disconnect, autocommit, commit/rollback,
direct and prepared execution, fetch/free-statement, cancellation, and bounded
catalog metadata for catalogs, schemas, tables, columns, and primary keys. The
repository T4 JDBC driver gate covers prepared reuse, typed results, overlapping
transactions, disconnect rollback, cancellation with an active peer, metadata,
and restart persistence. The driver's public `Statement.cancel()` does not
dispatch while a request is active; cancellation is therefore gated through
the driver's own internal `T4_Dcs_Cancel` path.
Requests enter concurrent connection
threads but compiler/executor work is deliberately serialized while the
embedded CLI remains process-global.

M12 defines `LocalLiteStorageEngine`, session, transaction, and streaming cursor
interfaces. A shared gate runs RocksDB TransactionDB and SQLite WAL through the
same atomicity, snapshot, conflict, cancellation, recovery, backup/restore,
integrity, metrics, and disk-watermark checks. TransactionDB is selected for
the single-node runtime. A synchronous TransactionDB journal is the durable
commit decision, all tables are conflict-checked before that decision, and
idempotent table markers allow startup to finish an interrupted multi-table SQL
commit. Metadata keys use collision-free version-2 encodings.

M13 switches the SQL runtime exclusively to `transactiondb/` format version 2.
Fresh stores publish `m13/active` and the versioned logical-layout marker before
catalog or table access. An interruption after the format marker is retryable.
The old per-table `catalog/` and `data/` format is rejected at startup; its
migration, export, rollback, and runtime fallback code is not shipped. The
synchronous M12 recovery journal remains separate while SQL publication still
uses its idempotent recovery protocol.

M14 TPC-C qualification is in progress. M14A pins the 5.11.0 input contract,
nine-entity mapping, deterministic one-warehouse scale, known deviations, and
a machine-readable baseline gate. M14B next creates and loads the real database,
followed by all five transaction profiles through T4 JDBC, Level 3
isolation evidence, concurrent compiler/executor work, and then a reproducible
multi-warehouse TPC-C-like workload. Results are not `tpmC` and do not claim
formal TPC-C compliance until all specification and disclosure requirements are
met. Upgrade/drain orchestration, service-level backup controls, journal
consolidation, node HA, distributed execution, and password/TLS authentication
remain separate productization work.

### Functional boundary

| Area | Validated local-lite surface | Not currently claimed |
| --- | --- | --- |
| DML and storage | Session-owned overlapping transactions; INSERT, UPDATE, DELETE, UPSERT, and MERGE; secondary indexes; backend-neutral transaction/cursor contract; crash-recoverable multi-table DML; checkpoint, backup/restore, integrity verification, disk-watermark checks, and exclusive unified TransactionDB layout | In-place upgrade from the removed per-table format, distributed transactions, node-level HA, online multi-process writers, and zero-downtime upgrade orchestration |
| Catalog and DDL | Schemas, views, synonyms, sequences, defaults, CHECK and bounded RI constraints, identity columns, triggers, and persisted basic statistics | RI CASCADE, computed system columns, full histograms, and unrestricted physical `_MD_` behavior |
| Types and executor | ISO88591/UTF8/UCS2, binary types, BOOLEAN, INTERVAL, LONG VARCHAR, cursors, window/grouping operations, local sort/scratch, and cancellation cleanup | LOB/ARRAY and broader collation support, ESP fan-out, and distributed execution |
| Authorization and UDR | Local users, roles, ownership/privileges, catalog identity validation at server startup, and bounded native/Java UDR adapters | Password/TLS/external identity services, the full UDR server, and host-rowset behavior |
| Runtime and clients | Standalone `sqlci`; multi-client `nativelite-server`; loopback TCP and protected 0600 Unix sockets; reduced Trafodion Type 4 association/SQL protocol tested with the repository T4 JDBC driver | Full T4/DCS compatibility, ODBC certification, LOB/call/batch/rowset input, broad catalog APIs, remote/TLS deployment, DCS/REST, and HBase/HDFS/Hive runtimes |

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

## Quick NativeLite Server Run

After applying the same `TRAF_HOME`, `TRAF_LOCAL_LITE`, store, and library-path
environment used above, start the local trusted endpoint with:

```bash
$SQL_LIBS/nativelite-server --listen 127.0.0.1 --port 23400
```

Connect with the Trafodion Type 4 JDBC driver
(`org.trafodion.jdbc.t4.T4Driver`):

```text
jdbc:t4jdbc://127.0.0.1:23400/:
```

This M11 endpoint intentionally has no password or TLS exchange. TCP binds are
restricted to numeric loopback addresses; alternatively pass
`--unix-socket /path/nativelite.sock`, which creates an owner-only socket for
lifecycle/embedding use; the current T4 JDBC gate uses TCP. User names must
already exist in the local catalog. Existing non-socket, foreign, or active
Unix paths are never replaced. Use this endpoint only as the documented local
trusted transport.

## Quick Regress Run

After building the local-lite SQL binary, run the native TEST/EXPECTED lane
without starting SQF, TMF, HBase, Hadoop, or ZooKeeper:

```bash
make local-lite-m13
```

This runs M12's common TransactionDB/SQLite contract and fault matrix, metadata
key migration, and real SQLCI multi-table commit interruption/restart recovery,
followed by M13's format-activation retry, old-layout rejection, unified-only
DDL/DML, and restart checks.

### Storage Format

M13 is a deliberate format break. Start the sole owner with a fresh store or a
store already created by this build:

```bash
TRAF_LOCAL_STORE_DIR=/path/to/store \
  $SQL_LIBS/nativelite-server --listen 127.0.0.1 --port 23400
```

Startup creates `/path/to/store/transactiondb` and publishes the format and
activation markers. If `/path/to/store/catalog` or `/path/to/store/data`
exists, startup fails explicitly. There is no old-layout reader, migration,
export, rollback, or runtime fallback; recovery uses TransactionDB
backup/restore. `TRAF_LOCAL_LITE_ACTIVATION_FAULT=after-format` is reserved for
the M13 fault gate and exits with status 91 before activation.

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
make local-lite-m11
make local-lite-legacy-audit
make local-lite-regress-inventory
```

The runner prints the temporary artifact directory containing `RAWnnn`,
`LOGnnn`, and `DIFFnnn` files. The cases and baselines live under
`core/sql/regress/localLite`.

Use `make all` for the normal full Trafodion build.
