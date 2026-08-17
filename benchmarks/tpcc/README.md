# NativeLite TPC-C Qualification Assets

This directory contains the versioned inputs for the NativeLite M14
qualification workload. The normative reference is TPC-C 5.11.0, available
from <https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf>.
The specification itself is not vendored.

These assets define a dialect-adapted, non-audited workload. Passing repository
gates permits the label `TPC-C-like`; it does not permit an official TPC-C or
`tpmC` claim.

## Versioned contract

- `qualification.properties` fixes the qualification scale, seeds, batching,
  terminal, retry, and reporting contract.
- `schema-manifest.tsv` maps the nine logical TPC-C entities to NativeLite table
  names and declares their keys and cardinalities.
- `dialect-deviations.tsv` records every known departure from the normative
  schema or benchmark procedure.

The repository-owned JDBC workload driver is implemented under `scripts/` and
uses the reduced NativeLite T4 endpoint. It is intentionally inspectable and is
not represented as a TPC-certified driver.

Run `make local-lite-m14a` to validate these inputs and emit a machine-readable
baseline report. Run `make local-lite-m14b` for the fast smoke-scale schema,
loader, interruption/retry, restart, and backup/restore gate. Set
`TPCC_M14_SCALE=qualification` to exercise the pinned one-warehouse scale:

```sh
TPCC_M14_SCALE=qualification make local-lite-m14b
```

The loader is deterministic and restartable at bounded commit boundaries. It
uses the real reduced T4 JDBC endpoint; its report contains the exact generated
cardinalities and post-load consistency result.

Run `make local-lite-m14c` for the five deterministic transaction profiles,
first in isolation and then as a two-terminal mix. The gate records classified
retries, negative diagnostics, rollback/disconnect cleanup, final consistency,
and restart persistence. This is a functional profile gate; the normative
random transaction mix, terminal pacing, and multi-warehouse scale belong to
M14F.

Run `make local-lite-m14d` for the versioned two-session Level 3 isolation and
durability matrix. The selected serializability mechanism combines stable
TransactionDB snapshots with optimistic validation of the database sequence at
commit. This prevents write skew and predicate phantoms but conservatively
aborts a writer after any concurrent database write, including an independent
one. Conflicts return the retryable `restart transaction` diagnostic without
lock waits or deadlock cycles. The gate also terminates the server before and
after the durable journal decision for New-Order, Payment, and Delivery and
checks atomic effects after restart.

Run `make local-lite-m14e` to prove that independent connection threads enter
the real compiler/executor region concurrently. The gate records a minimum
observed depth of two for five repeated races, checks per-session schema and
diagnostic isolation, and reruns the T4 cancellation, disconnect rollback,
peer-survival, and restart suite. The ownership/synchronization audit is
versioned in `m14e-runtime-inventory.tsv`. DDL/catalog changes and SQLCI
compatibility utilities remain narrowly serialized; requests do not use the
engine initialization queue.

Run `make local-lite-m14f` for the fixed two-warehouse operations scale: two
districts and 100 customers/orders per warehouse, 1,000 shared items, two
terminal sessions, 5 warmup and 20 measured transactions per terminal, and two
repetitions with a maximum throughput variance ratio of 0.75. The deterministic
mix is 45% New-Order, 40% Payment, and 5% each Order-Status, Delivery, and
Stock-Level. It has no normative think-time pacing, last-name branch, or
variable order-line selection and is therefore explicitly non-compliant.

The workload reports committed, aborted, and retried counts plus p50/p95/p99/max
client end-to-end latency (including admission wait) for every profile and
aggregate transactions per second. Its two
sessions use fair client-side writer admission because the current
database-wide serializable validator would otherwise reject independent
warehouse writers; read-only statements retain the M14E concurrent path. The
operations report records source/runtime identity, RSS high-water, store-size
delta,
clean/unclean/checkpoint recovery time, an online TransactionDB checkpoint made
during workload execution, restored-store consistency, and disk-watermark
rejection. Queue, compile, WAL/fsync, compaction, write-stall, and cache metrics
are marked unavailable rather than inferred from the reduced server or RocksDB
C API.

Run `make local-lite-m14` for the aggregate M14G gate. It keeps M10-M13 and
M14A-M14F as explicit prerequisites, retains the M14F artifacts under
`M14_REPORT_DIR` (default `/tmp/traf-local-lite-m14-report`), and writes
`qualification-report.json` there. `m14-regression-inputs.tsv` is the required
gate inventory; `claim-checklist.tsv` independently records functional support,
repeatable TPC-C-like support, and failed formal-compliance requirements. The
aggregate report embeds the actual workload and operations reports, revision
hashes, isolation/crash summary, deviation-manifest reference, and the explicit
`official_tpmc: not_claimed` boundary.

## M15 Trafodion MVCC/OCC contract

M15 replaces M14D's database-wide sequence equality check with the original
Trafodion optimistic validation model. A transaction reads from one unified
TransactionDB snapshot, records point and predicate ranges, and at commit
checks only write keys published after its start sequence. Writes are also
reads for conflict purposes, matching `TrxTransactionState` write ordering.

`occ-contract.tsv` is the machine-readable isolation contract and
`occ-metrics.tsv` fixes the counters required in the M15 qualification report.
This design is not SSCC and does not select RocksDB OptimisticTransactionDB;
TransactionDB remains the snapshot and atomic persistence layer. Run
`make local-lite-m15a` to validate the contract against the retained Trafodion
reference implementation.

M15A-M15G are complete. The phases add the contract and metrics, one unified
transaction snapshot, OCC read/write sets, post-start validation, transactional
secondary-index reads, atomic delta publication, and Release qualification.
Run `make local-lite-m15g`; artifacts are written to
`/tmp/traf-local-lite-m15-report` by default. The deterministic qualification
scale is 32 warehouses and 32 independently offset terminals over 10 districts,
100 customers and orders per district, 30 new orders per district, and 1,000
items. It is deliberately a repeatable TPC-C-like engineering scale, not the
formal TPC-C population or an official `tpmC` run.

The 2026-08-16 Release baseline passes at 1.130 TPS, 3.423% three-run variance,
zero conflicts/retries/unclassified errors, and 161-214 ms recovery. Five
profiles have non-zero samples. Stock-Level p95 is 135.075 s and dominates the
current gap; the configuration records permissive regression thresholds
separately from the 50 TPS and 1/0.5/0.5/2/2 s production targets.

## M16 Stock-Level optimization

M16 targets the Stock-Level p95 bottleneck with an ordered secondary index and
a two-phase transaction path. `stock-level-contract.tsv` is normative for the
query shape and `m16-stock-level.properties` fixes the optimization gate.
The first phase ranges over the most recent 20 orders using
`TPCC_ORDER_LINE_STOCK_IX`, deduplicates supply-warehouse/item pairs, and the
second phase performs `TPCC_STOCK` primary-key point reads under the same
MVCC/OCC snapshot. The result must remain equivalent to the original join and
Stock-Level full scans are disallowed by the M16 gate.

The M16 implementation, SQLCI optimizer proof, staged Makefile gates, and
Release runtime evidence are complete. The latest repeatable run measured
19.516 TPS with 0.012002 variance; Stock-Level p95 was 1.292 s with zero
Stock-Level full scans. New-Order remained the main latency bottleneck at
1.992 s p95, so M17 targets its T4 execution path and OCC validation cost.

## M17 New-Order and OCC validation optimization

M17 is specified in `plan/local-lite-tpcc-m17-design.md`. New-Order now reads
the district sequence, warehouse tax, and customer discount through one
prepared join in the same transaction snapshot, reducing T4 round trips while
retaining an exact-one-row check. The OCC coordinator also maintains an
object-UID history index and validates only writes that can overlap the current
transaction's point/range read set; cleanup and history-overflow behavior stay
in lockstep with the existing committed history.

The staged implementation gate is `make local-lite-m17` (A: source/design
contract, B: T4 and five-profile correctness, C: Release performance). The
2026-08-17 M17 run passed all gates at 19.667 TPS and 1.884 s New-Order p95,
with zero conflicts, retries, unclassified errors, and Stock-Level full scans.
M17 retains the 50 TPS and New-Order p95 <=1 s engineering targets as measured
targets, not claims. It does not change the MVCC/OCC isolation contract, add
SSCC, or make an official TPC-C/`tpmC` claim.
