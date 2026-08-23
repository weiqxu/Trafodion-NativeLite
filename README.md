# Trafodion Lite Storage

This checkout adds **Lite Storage**, a RocksDB-based single-node storage path
for the native Trafodion SQL engine.

The `lite` build selects Lite Storage and removes Java, Maven, Hadoop, HDFS,
HBase, Hive, DCS, REST, TrafCI, and Java client modules from that build path.
The supported runtimes are standalone `sqlci` and the M11
`nativelite-server`; both use the normal native compiler/executor over the
embedded RocksDB catalog and table store.

The current single-node runtime backed by Lite Storage is not a complete
standalone database. Its bounded surface includes transactions, DDL/DML,
indexes, metadata/statistics,
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
plan/lite.md
```

The original Apache Trafodion project overview is preserved in:

```text
plan/README.trafodion.md
```

## Current Status (verified 2026-08-20)

The bounded M1-M15 scope is complete for its declared boundaries. Each
`ContextCli` owns an `ExTransaction`,
which is now the canonical owner of that session's `LiteTxnContext`;
executor/DDL paths receive it explicitly, and reset, disconnect, or destruction
discard only that session's pending writes and snapshots. The M11A gate covers
two overlapping CLI contexts, isolation, independent completion, deterministic
same-key conflict, reset/delete cleanup, snapshot release, and context reuse.
The effective authorization identity and new-object ownership also come from
the current `ContextCli`, including after `SET SESSION AUTHORIZATION`; the
compiler session is a propagated mirror rather than the Lite authority.

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
Requests execute on their owning connection threads. CLI current context,
SQLCI environment, assertion target, and default schema are thread-local while
the ContextCli, compiler, diagnostics, transaction, and statement state remain
session-owned. DDL/catalog mutations and SQLCI compatibility utilities retain
narrow locks; independent DML and queries compile and execute concurrently.

M12 defines `LiteStorageEngine`, session, transaction, and streaming cursor
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

M14 TPC-C-like qualification is complete. M14A pins the 5.11.0 input contract,
nine-entity mapping, deterministic one-warehouse scale, known deviations, and
a machine-readable baseline gate. M14B adds the real nine-table schema, a
bounded and restartable deterministic loader through T4 JDBC, exact cardinality
and relationship checks, plus clean-restart and copied-store restore proof.
M14C implements deterministic New-Order, Payment, Order-Status, Delivery, and
Stock-Level profiles through reusable prepared statements on T4 JDBC. Its
two-terminal gate classifies and retries optimistic commit conflicts, covers
rollback/disconnect/duplicate diagnostics, and rechecks effects after restart.
M14D adds a two-session Level 3 matrix for dirty read/write,
non-repeatable read, phantom, predicate conflict, and write skew. Stable
snapshots plus conservative database-sequence validation provide serializable
optimistic aborts without lock waits or deadlock cycles; a bounded retry proves
exactly-once logical effects. New-Order, Payment, and Delivery are each killed
before and after the durable journal decision and pass atomic restart checks.
This database-wide validator can abort independent writers and is intentionally
disclosed as a correctness-first M14D boundary. M14E removes the global engine
queue, proves an observed compiler/executor depth of at least two across five
instrumented read races, isolates session schemas and diagnostics, and retains
the T4 cancellation/disconnect/peer-survival gate. M14F supplies a reproducible
two-warehouse, two-terminal TPC-C-like workload with per-profile latency and
retry counters, throughput variance, resource usage, online checkpoint,
clean/unclean restart, checkpoint restore, and disk-watermark evidence. The
workload uses fair client-side writer admission because M14D's database-wide
validator produces false independent conflicts; the server's M14E concurrent
compiler/executor path remains unchanged.
M14G adds `make lite-m14`, explicitly reruns M10-M13 and M14A-M14F,
composes `qualification-report.json`, and publishes separate functional,
repeatable TPC-C-like, and formal-compliance checklists. Results are not `tpmC`
and do not claim
formal TPC-C compliance until all specification and disclosure requirements are
met. Upgrade/drain orchestration, service-level backup controls, journal
consolidation, node HA, distributed execution, and password/TLS authentication
remain separate productization work.

M15 replaces M14's database-wide validation and client writer admission with
transaction-wide TransactionDB snapshots and Trafodion MVCC/OCC read/write-set
validation. Point keys, primary/index ranges, predicate scans, index reads, and
writes participate in commit validation; disjoint deltas publish atomically
without rewriting an entire database image. `make lite-m15g` passes the
Release 32-warehouse/32-terminal five-profile gate with no client admission,
conflict, retry, or unclassified error. The verified 2026-08-16 baseline is
1.130 TPS with 3.423% three-run variance; p95 is 3.301 s New-Order, 1.312 s
Payment, 0.876 s Order-Status, 1.672 s Delivery, and 135.075 s Stock-Level.
This completes M15's correctness and repeatability scope, but is far below the
50 TPS and 1/0.5/0.5/2/2 s production targets. It is not a `tpmC` result or a
production-readiness claim.

M16 was the Stock-Level optimization milestone. It removes the Stock-Level
join/full-scan bottleneck through `TPCC_ORDER_LINE_STOCK_IX`, a range scan over
recent order lines, distinct item aggregation, and stock primary-key point
reads in one OCC transaction snapshot. The M16 gate requires join-equivalent
results and zero Stock-Level full scans; the 50 TPS and 2-second production
targets remain explicit targets until measured.

M16 implementation, plan validation, and Release runtime qualification are
complete. The latest run measured 19.516 TPS with 0.012002 variance and
Stock-Level p95 1.292 s with zero Stock-Level full scans; New-Order p95 remains
1.992 s. M17 is the active optimization milestone: it coalesces New-Order
header and ITEM/STOCK batch reads into prepared joins and indexes committed OCC
writes by object UID so validation avoids unrelated history entries. The latest
run measured 21.332 TPS and 1.479 s New-Order p95. Its design and staged
gates are in `plan/lite-tpcc-m17-design.md`; run `make lite-m17`.

M18 is now the active follow-up for T4 transaction-control and durable
publication overhead. It uses the Lite transaction participant for
initialized T4 `BEGIN`/`COMMIT`/`ROLLBACK` requests, preserves fallback for
first-use and DDL contexts, and keeps synchronous RocksDB commit enabled by
default. The latest sync run measured 19.464 TPS with 1.744 s New-Order p95;
an async-only diagnostic reduced aggregate publication latency from 3.928 s to
0.144 s but raised throughput only to 20.173 TPS. These remain TPC-C-like
engineering measurements, not official `tpmC` or production-readiness claims.

M19 implements the next five execution-path optimizations: prepared T4
parameter-template reuse, transaction ReadOptions reuse, secondary-index
MultiGet, a bounded unified-RocksDB block cache/Bloom policy, and prepared
rowset INSERT batching. The cache is sized by
`TRAF_LITE_BLOCK_CACHE_BYTES` (`0` default; a positive value enables
it), while synchronous commit remains the production default. The reduced T4
protocol
still batches homogeneous INSERT rows; heterogeneous New-Order statements
remain separate because no portable multi-statement request frame exists.
M19 design and validation commands are recorded in
[`plan/lite-tpcc-m19-design.md`](plan/lite-tpcc-m19-design.md).

M20 retains server-side prepared plans and typed input buffers, supports
quoted-string-aware statement batches, and keeps keyed parameter fallback
explicit. M21 moves each connection onto its owning thread with bounded
session capacity and a narrow legacy compiler boundary; its native loader
established the complete 10-warehouse input path.

M22 full-cardinality qualification is complete for phases A-G. Exact-key OCC
validation, deterministic commit intents, concurrent atomic TransactionDB
publication, resumable parallel loading, bound primary-key plans, static
primary multi-get/update plans, and compatible SELECT batching remove the six
recorded concurrency bottlenecks. The fresh Release run loaded 8,990,118 keys
in 100.014 seconds and measured 54.304 TPS over three 32-terminal repetitions
(54.900/54.298/53.728 TPS, 2.1806% variance). New-Order, Payment,
Order-Status, Delivery, and Stock-Level p95 were 892.390, 467.642, 355.940,
1256.184, and 475.943 ms. Synchronous commit, consistency, checkpoint,
clean/unclean restart, restore, and disk-watermark gates passed. This remains
TPC-C-like engineering evidence, not official tpmC. The clean-tree M22H audit
covers Debug/Release builds, M10-M22, native and legacy allowlist regressions,
T4, OCC/isolation, fault recovery, and a revision-bound full qualification.
Run `make lite-m22`.

### Functional boundary

| Area | Validated Lite Storage-backed surface | Not currently claimed |
| --- | --- | --- |
| DML and storage | Session-owned overlapping transactions; INSERT, UPDATE, DELETE, UPSERT, and MERGE; secondary indexes; backend-neutral transaction/cursor contract; crash-recoverable multi-table DML; checkpoint, backup/restore, integrity verification, disk-watermark checks, and exclusive unified TransactionDB layout | In-place upgrade from the removed per-table format, distributed transactions, node-level HA, online multi-process writers, and zero-downtime upgrade orchestration |
| Catalog and DDL | Schemas, views, synonyms, sequences, defaults, CHECK and bounded RI constraints, identity columns, triggers, and persisted basic statistics | RI CASCADE, computed system columns, full histograms, and unrestricted physical `_MD_` behavior |
| Types and executor | ISO88591/UTF8/UCS2, binary types, BOOLEAN, INTERVAL, LONG VARCHAR, cursors, window/grouping operations, local sort/scratch, and cancellation cleanup | LOB/ARRAY and broader collation support, ESP fan-out, and distributed execution |
| Authorization and UDR | Lite users, roles, ownership/privileges, catalog identity validation at server startup, and bounded native/Java UDR adapters | Password/TLS/external identity services, the full UDR server, and host-rowset behavior |
| Runtime and clients | Standalone `sqlci`; multi-client `nativelite-server`; loopback TCP and protected 0600 Unix sockets; reduced Trafodion Type 4 association/SQL protocol tested with the repository T4 JDBC driver | Full T4/DCS compatibility, ODBC certification, LOB/call/batch/rowset input, broad catalog APIs, remote/TLS deployment, DCS/REST, and HBase/HDFS/Hive runtimes |

The detailed milestone evidence and boundaries are maintained in
[`plan/lite-legacy-regress-roadmap.md`](plan/lite-legacy-regress-roadmap.md).
The separate newregress qualification order is in
[`plan/lite-newregr-roadmap.md`](plan/lite-newregr-roadmap.md).

### Regression snapshot

| Test surface | Inventory | Current evidence |
| --- | ---: | --- |
| Native Lite Storage lane | 43 TEST/EXPECTED cases | **43/43 pass**, zero non-empty DIFF files |
| Unmodified Trafodion legacy allowlist | 11 cases | **11/11 pass**, zero non-empty DIFF files |
| Audited Trafodion legacy inventory | 122 unique primary TEST inputs from 9 standard suites | 11 runnable, 51 blocked, 42 unsafe, and 18 excluded |
| Remaining standard suites | 14 Hive and 25 QAT cases | Hive explicitly excluded; QAT classified as blocked/unassessed pending a shared-state adapter |
| Separate `newregr` inventory | 281 statically paired cases and 1 unpaired MVS input, plus custom performance workloads | No lite execution/convergence result yet |

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
in [`reprobe-m2-m6-2026-08-12.tsv`](core/sql/regress/liteLegacy/reprobe-m2-m6-2026-08-12.tsv).
Unsafe entries generally require shell commands, multiple SQLCI processes,
helper binaries, Java, or the Trafodion service stack; excluded entries are
predominantly physical HBase/Hive behavior.

No current full `runallsb`, Hive, QAT, or `newregr` run is being claimed. Those
surfaces are unassessed, not failed. See the
[`liteLegacy` adapter README](core/sql/regress/liteLegacy/README.md)
for the inventory, safety rules, and probe workflow.

## Quick Build

Install native dependencies:

```bash
scripts/install-lite-deps.sh --dry-run
scripts/install-lite-deps.sh -y
```

Build lite:

```bash
OMPI_CXX=/usr/bin/g++ make lite
```

## Quick SQLCI Run

For the current repository build layout:

```bash
export TRAF_HOME=$(pwd)/core/sqf
export TRAF_LITE=1
export TRAF_LITE_STORE_DIR=${TRAF_LITE_STORE_DIR:-/tmp/traf-lite-store}

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

Minimal Lite table smoke example:

```sql
>>CREATE TABLE t(a INT, b VARCHAR(20));
>>INSERT INTO t VALUES (1, 'one'), (2, 'two');
>>SELECT * FROM t;
>>DROP TABLE t;
>>exit;
```

## Quick NativeLite Server Run

After applying the same `TRAF_HOME`, `TRAF_LITE`, store, and library-path
environment used above, start the local trusted endpoint with:

```bash
$SQL_LIBS/nativelite-server --listen 127.0.0.1 --port 23400
```

The same build also provides a small SQLCI-style network client:

```bash
$SQL_LIBS/nativelite-client \
  --host 127.0.0.1 --port 23400
```

It accepts one or more SQL statements terminated by `;`, supports multiline
input and `exit`, `quit`, or `\\q`, and can execute a script with `-f`:

```bash
$SQL_LIBS/nativelite-client \
  --host 127.0.0.1 --port 23400 -f smoke.sql
```

The client speaks the reduced Trafodion Type 4 protocol exposed by the Lite
Storage-backed runtime directly;
it is not a PostgreSQL-wire client. It currently implements direct SQL,
fetching and tabular result display, transaction-independent disconnect, and
SQL diagnostics. The endpoint remains the local trusted transport described
below: it has no password or TLS exchange.

Connect with the Trafodion Type 4 JDBC driver
(`org.trafodion.jdbc.t4.T4Driver`):

```text
jdbc:t4jdbc://127.0.0.1:23400/:
```

This M11 endpoint intentionally has no password or TLS exchange. TCP binds are
restricted to numeric loopback addresses; alternatively pass
`--unix-socket /path/nativelite.sock`, which creates an owner-only socket for
lifecycle/embedding use; the current T4 JDBC gate uses TCP. User names must
already exist in the Lite catalog. Existing non-socket, foreign, or active
Unix paths are never replaced. Use this endpoint only as the documented local
trusted transport.

## Quick Regress Run

After building the lite SQL binary, run the native TEST/EXPECTED lane
without starting SQF, TMF, HBase, Hadoop, or ZooKeeper:

```bash
make lite-m13
```

This runs M12's common TransactionDB/SQLite contract and fault matrix, metadata
key migration, and real SQLCI multi-table commit interruption/restart recovery,
followed by M13's format-activation retry, old-layout rejection, unified-only
DDL/DML, and restart checks.

### Storage Format

M13 is a deliberate format break. Start the sole owner with a fresh store or a
store already created by this build:

```bash
TRAF_LITE_STORE_DIR=/path/to/store \
  $SQL_LIBS/nativelite-server --listen 127.0.0.1 --port 23400
```

Startup creates `/path/to/store/transactiondb` and publishes the format and
activation markers. If `/path/to/store/catalog` or `/path/to/store/data`
exists, startup fails explicitly. There is no old-layout reader, migration,
export, rollback, or runtime fallback; recovery uses TransactionDB
backup/restore. `TRAF_LITE_ACTIVATION_FAULT=after-format` is reserved for
the M13 fault gate and exits with status 91 before activation.

```bash
make lite-regress
```

Run selected cases with:

```bash
make lite-regress LITE_REGR_TESTS="001 003"
```

Run the bounded M10 convergence gate (all 43 native cases plus the complete
eleven-case legacy allowlist) and inspect both the adapted and complete upstream
inventories with:

```bash
make lite-m10
make lite-m11
make lite-legacy-audit
make lite-regress-inventory
```

The runner prints the temporary artifact directory containing `RAWnnn`,
`LOGnnn`, and `DIFFnnn` files. The cases and baselines live under
`core/sql/regress/lite`.

Use `make all` for the normal full Trafodion build.
