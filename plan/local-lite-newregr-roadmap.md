# Local-Lite Newregress Qualification Roadmap

## Purpose And Accounting Boundary

This plan qualifies the historical `core/sql/regress/newregr` assets for
local-lite without mixing them into the standard `runallsb` pass rate. The
standard inventory contains 161 logical cases. Newregress is a separate asset
family with different naming, state, clients, runners, and expected-output
contracts.

The authoritative static summary is
`core/sql/regress/localLiteLegacy/newregr-inventory.tsv`. Regenerate and inspect
it with:

```bash
make local-lite-regress-inventory
scripts/audit-local-lite-upstream-regress.sh --list-newregr
```

The current source tree contains 281 paired inputs and one unpaired input. No
newregress execution result is currently claimed.

| Suite | Paired | Unpaired | Qualification boundary |
| --- | ---: | ---: | --- |
| `card` | 22 | 0 | Stateful cardinality schema/load and output adapter required |
| `mvqr` | 19 | 0 | Materialized-view query-rewrite services are outside the current runtime |
| `mvs` | 44 | 1 | Materialized views are outside the current runtime; `TESTMV500A` has no baseline |
| `opt` | 11 | 0 | Optimizer plan baselines need local normalization and a suite adapter |
| `parallel` | 6 | 0 | Requires ESP/distributed execution, outside the single-process surface |
| `rowsets` | 44 | 0 | Requires a rowset-capable client/CLI harness rather than plain SQLCI |
| `triggers` | 135 | 0 | Requires ordered shared state and section-level safety review |
| `exeperf`, `perf` | 0 | 0 | Custom performance workloads, not static TEST/EXPECTED contracts |

## Qualification Rules

Each executable case must have a versioned manifest row that records its input,
baseline, state owner, required helper, safety classification, and disposition.
Passing requires a zero client status and an empty normalized DIFF. Process exit
zero alone is insufficient. Filters may normalize dynamic values but must not
remove semantic diagnostics, plan operators, or result rows.

Stateless cases receive an isolated `TRAF_LOCAL_STORE_DIR`. A suite whose cases
intentionally share DDL/data state receives one isolated store for that ordered
suite run, never a process-global developer store. Shell, Java, native-client,
and service-stack dependencies must be declared before execution. Unpaired
inputs remain explicit inventory blockers rather than implicit skips.

## Execution Sequence

### N0: Static inventory — complete

The audit discovers paired inputs from each suite's native naming convention,
checks `newregr-inventory.tsv`, reports the missing generic
`tools/runregr_other.ksh`, and keeps `perf`/`exeperf` outside the case count.

### N1: Portable SQLCI candidates — next

Build explicit adapters for `opt` and `card` first. Reconstruct their setup,
load, execution order, filter, and baseline-selection rules inside isolated
stores. Probe one small case from each suite before expanding the manifest.
Plan-sensitive differences remain blocked until the operator and cardinality
semantics are reviewed; they are not accepted by copying current output.

After those runners are stable, statically split the `triggers` inventory into
portable SQL, helper-dependent, and unsupported trigger semantics. Preserve one
ordered store for related setup/test/cleanup groups. Local native `TEST036`
proves only the bounded trigger surface and does not pre-qualify these 135 cases.

### N2: Client-specific rowsets

Define a rowset-capable local client harness and prove prepare/bind/execute/fetch
contracts independently of a future network protocol. Until that exists, all 44
paired `rowsets` assets remain unassessed. Do not translate them into unrelated
scalar SQLCI tests merely to increase the count.

### N3: Product-bound suites

Keep `mvs` and `mvqr` blocked until materialized-view storage, refresh, metadata,
and rewrite lifecycle are explicit product features. Resolve the missing
`TESTMV500A` baseline before admitting the MVS suite to a gate. Keep `parallel`
blocked until a supported ESP/distributed execution boundary exists. M11's
multi-session server alone does not satisfy this requirement.

### N4: Performance qualification

Only after functional adapters are stable, define reproducible datasets,
resource limits, warmup, repetition, and variance thresholds for `perf` and
`exeperf`. Report them as performance observations, never as additional
TEST/EXPECTED passes.

## Gate Shape

Newregress should eventually expose a dedicated command such as
`make local-lite-newregr`, with per-suite selection and retained artifacts. It
must remain separate from `make local-lite-m10` and from the 161-case standard
inventory report. The gate may become required only after every selected case
has an audited dependency boundary and repeatable baseline.
