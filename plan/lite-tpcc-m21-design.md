# Trafodion Lite M21: Thread-Affine Multi-Worker Execution

Status: implementation complete for the multi-worker/session-isolation gate,
session-local plan reuse, and the native TPCC bulk-loader; the complete
qualification workload is exposed as `make lite-m21-tpcc`.

## Goal and boundary

M21 removes the process-wide request execution bottleneck while retaining the
legacy compiler's safety requirements. Each accepted T4 connection owns one
CLI context, `SqlciEnv`, compiler context, prepared-plan set, transaction state,
and diagnostic capture. Requests for that connection execute on its connection
thread for the full session lifetime.

The server is still a bounded loopback Trafodion Lite runtime. It does not claim
formal TPC-C compliance or official tpmC. HDFS/Hive/HBase and the distributed
SQF service stack remain outside scope.

## Execution model

```text
accept -> handshake/session slot -> connection thread
                                      |
                                      +-- switch to session ContextCli
                                      +-- compile plan (legacy compiler gate)
                                      +-- execute/fetch/commit concurrently
                                      +-- session-local diagnostics/state
                                      +-- destroy after cancel quiescence
```

The old `TrafodionLiteEngine` request queue is no longer used for client work.
`submit()` keeps a local request envelope for error/result plumbing, then
dispatches directly on the owning connection thread. The bootstrap engine
thread only owns the process default CLI context and Lite Storage lease.

The legacy optimizer/compiler has process-global mutable state. M21 therefore
serializes only `fetchRowsPrologue()`/plan construction with
`compilerMutex_`. Compilation is not the execution worker: after a plan is
constructed, executor fetch, transaction publication, RocksDB work, and
independent sessions proceed concurrently. CmpContext construction remains
protected by the existing CLI bootstrap semaphore for the same reason.

## Session safety

- `Session::ownerThread` rejects cross-thread use with SQLSTATE `08003`.
- `closing`, `activeCancels`, and a condition variable make destroy wait for
  an in-flight cancel before deleting the context.
- Cancel switches only to the target session context and returns to the
  caller's previous context.
- Session schema, transaction status, failed-transaction state, diagnostics,
  and prepared plans are not shared between connections.
- Automatic plan reuse is session-local and exact-SQL keyed. It covers
  parameter-free SELECT/INSERT/UPDATE/DELETE/MERGE/UPSERT statements, keeps an
  LRU of 64 plans, evicts on execution errors, and is cleared before catalog
  DDL. Parameterized T4 prepared statements continue through the explicit
  prepare/execute protocol and are never shared across sessions.
- `--workers N` bounds live sessions (`1 <= N <= 256`), with a default derived
  from hardware and capped at 32.
- A rejected SQLCONNECT uses the T4 `InitializeDialogueReply` SQL error list
  and SQLSTATE `53300`; after a session closes, its slot is reusable.

## Shared-state rules

The stored-procedure registry is initialized once with `std::call_once`.
Compiler authorization state and emergency new-handler storage are thread-local
for embedded Trafodion Lite contexts. The Lite Storage/OCC publication path
continues to use its storage mutex and transaction context; ordinary DML and
executor requests are not routed through a single server queue.

## Test contract

`scripts/test-lite-m21-concurrency.sh` proves:

- 32 independent JDBC sessions enter the executor concurrently;
- session schema isolation and diagnostic isolation;
- peer survival after an invalid statement;
- worker-capacity rejection and slot reuse.

The complete TPCC command is:

```bash
make lite-m21-tpcc
```

It uses `benchmarks/tpcc/m21-10w32c.properties`, loads qualification-scale
10-warehouse data, runs 32 terminals, and retains workload/operations reports
under `M21_REPORT_DIR`. The test service uses 64 session slots because the
workload has 32 business terminals plus loader/coordinator/checkpoint/recovery
connections; business concurrency remains 32.

The native loader is `trafodion-lite-bulk-loader`. It creates no SQL client
sessions and opens the same Lite Storage only after schema creation has
closed the server. It encodes deterministic TPCC rows with the Lite Storage row
codec, stages them with `LiteTxn::insertRows`, and commits through the
unified OCC/index/FK publication path. Warehouse row generation is parallel;
publication batches are serialized by an explicit loader mutex because the
current unified TransactionDB publication path is process-wide. This keeps
worker/session isolation without claiming parallel physical commits. The full
qualification target uses `TPCC_NATIVE_COMMIT_ROWS=10000` to bound the
ORDER_LINE commit working set; the loader also accepts `--commit-rows N` for
controlled experiments.

## Known performance boundary

At 10 warehouses and 32 terminals, the OCC conflict rate is materially higher
than the earlier two-warehouse/two-terminal comparison. Client-visible latency
includes OCC retry backoff. The M21 report must preserve observed p50/p95/p99,
retry counts, conflict counts, throughput, and recovery results; relaxing a
qualification threshold is not a performance claim. The current native
loader improves staging/encoding cost and avoids the per-row pending duplicate
scan, but unified publication remains serialized and should be the next
bulk-load performance target.
