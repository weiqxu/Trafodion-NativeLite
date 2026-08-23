# Lite Storage Legacy Regress Adapter

This directory inventories and probes legacy Trafodion regress inputs without
modifying their TEST or EXPECTED files and without starting the Trafodion
service stack.

The adapter is a baseline and migration tool. The native, gating lite
suite remains `../lite`.

## Manifest

`manifest.tsv` contains these tab-separated columns:

1. suite
2. TEST name
3. selector (`all` or a comma-separated section sequence)
4. classification
5. target milestone
6. disposition
7. blocker
8. source evidence

Default execution is restricted to `runnable` entries. `--probe` permits an
explicit safe `blocked` or `needs-review` entry so its current first failure can
be recorded. `unsafe` and `excluded` entries are never executed.

The M10A-M10F convergence gate is:

```bash
make lite-m10
```

It runs the complete runnable allowlist and then the complete native lite
lane. It also checks the explicit M10A-M10F evidence cases (DDL, statistics,
DML/indexes, character sets/types, advanced executor, and authorization/UDR).
A fresh case count is derived from the checked-in `TESTnnn` files, so adding a
paired TEST/EXPECTED case cannot leave this gate pinned to an obsolete total.
A blocked entry is not treated as a pass; promote it only after its
RocksDB-compatible semantics and normalized EXPECTED output have been reviewed.

The current allowlist contains eleven entries. The 2026-08-12 M2-M6 re-probe
executed all 49 safe blocked sections, then reran all initial timeouts with a
120-second per-entry limit: five matched exactly and were promoted, 29 retained
output differences, 13 timed out, and two aborted in string result rendering.
The row-level snapshot
is `reprobe-m2-m6-2026-08-12.tsv`; it records execution evidence, not a license
to replace a legacy baseline with current output.

## Commands

Validate inventory completeness and print the current report:

```bash
make lite-legacy-audit
```

Audit the complete standard `runallsb` surface and the separate `newregr`
assets with:

```bash
make lite-regress-inventory
scripts/audit-lite-upstream-regress.sh --list-newregr
```

`standard-extra-manifest.tsv` explicitly classifies the 14 Hive cases and 25
QAT cases omitted from `manifest.tsv`. Hive is excluded because lite
removes that runtime. QAT remains blocked/unassessed because its DDL, load, and
DML cases share one ordered schema/data lifecycle; an adapter must preserve that
state before any QAT result can be claimed.

`newregr-inventory.tsv` is deliberately separate from the standard pass rate.
The audit discovers 281 paired inputs, one unpaired MVS input (`TESTMV500A`),
and the custom `perf`/`exeperf` workloads. No newregr case is runnable yet: the
generic tools path falls back to an absent `runregr_other.ksh`, and each suite
still needs an explicit state, dependency, and baseline-normalization adapter.
The staged execution strategy is documented in
`plan/lite-newregr-roadmap.md`.

Inspect static feature and safety candidates:

```bash
scripts/audit-lite-legacy-regress.sh --candidates
```

List manifest entries:

```bash
core/sql/regress/liteLegacy/runregr --list --suite core
```

Probe an explicit safe blocked entry:

```bash
core/sql/regress/liteLegacy/runregr \
  --probe --suite charsets TEST012
```

Each executed entry receives a separate temporary RocksDB store. RAW, LOG,
normalized EXPECTED, DIFF, SAFETY, and RESULT artifacts are retained under the
reported `/tmp/traf-lite-legacy.*` directory unless `--run-dir` or
`LITE_LEGACY_RUN_DIR` selects another location.

## Safety

Before SQLCI starts, the adapter rejects selected input containing active shell
commands, `TESTABORT`, the known-crashing `cleanup obsolete volatile tables`
command, or a macro-based external OBEY path such as
`$$TRAF_HOME$$/sql/scripts/regrinit.sql`. Section-specific entries are checked
using only their selected section bodies. Selected bodies are materialized in
manifest order, which supports legacy section names that are not valid in an
`OBEY TEST(section)` statement and repeated cleanup sections. The manifest
audit also requires whole TEST inputs with known unsafe directives to be marked
`unsafe` or `excluded`.

Do not weaken the safety scan or add broad output filtering to turn an
unsupported feature into a passing result. Split mixed TEST files into reviewed
section entries and retain a concrete exclusion or blocker reason.
