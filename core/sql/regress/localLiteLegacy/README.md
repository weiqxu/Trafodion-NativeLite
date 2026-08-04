# Local-Lite Legacy Regress Adapter

This directory inventories and probes legacy Trafodion regress inputs without
modifying their TEST or EXPECTED files and without starting the Trafodion
service stack.

The adapter is a baseline and migration tool. The native, gating local-lite
suite remains `../localLite`.

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
make local-lite-m10
```

It runs the complete runnable allowlist and then the native 42-test local-lite
lane. It also checks the explicit M10A-M10F evidence cases (DDL, statistics,
DML/indexes, character sets/types, advanced executor, and authorization/UDR).
A blocked entry is not treated as a pass; promote it only after its
RocksDB-compatible semantics and normalized EXPECTED output have been reviewed.

## Commands

Validate inventory completeness and print the current report:

```bash
make local-lite-legacy-audit
```

Inspect static feature and safety candidates:

```bash
scripts/audit-local-lite-legacy-regress.sh --candidates
```

List manifest entries:

```bash
core/sql/regress/localLiteLegacy/runregr --list --suite core
```

Probe an explicit safe blocked entry:

```bash
core/sql/regress/localLiteLegacy/runregr \
  --probe --suite charsets TEST012
```

Each executed entry receives a separate temporary RocksDB store. RAW, LOG,
normalized EXPECTED, DIFF, SAFETY, and RESULT artifacts are retained under the
reported `/tmp/traf-local-lite-legacy.*` directory unless `--run-dir` or
`LOCAL_LITE_LEGACY_RUN_DIR` selects another location.

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
