# Seabase Legacy Review

This file records the Milestone 0 review of the eleven remaining safe
`needs-review` entries in the `seabase` suite.

## Probe Method

Eight entries that were not wholly HBase-specific were run through the isolated
adapter with a 30 second per-test ceiling:

```sh
LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/liteLegacy/runregr --probe --suite seabase \
  TEST011 TEST012 TEST020 TEST024 TEST025 TEST030 TEST032 TEST033
```

`TEST001`, `TEST027`, and `TEST040` were excluded by source review before
execution because HBase versions/timestamps, column families, or SALT storage
are their primary purpose.

The M2-M6 rows below were refreshed from the 2026-08-12 re-probe; every initial
timeout was retried with a 120-second limit. See
`reprobe-m2-m6-2026-08-12.tsv` for row-level status.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | excluded | - | HBase versions, timestamps, and physical options are the complete test purpose |
| TEST011 | unsafe | M7 | Mixed repository/HBase DML test terminates SQLCI with status 139 after the session |
| TEST012 HBase sections | excluded | - | Serialization, coprocessor, and HBase filter-pushdown coverage |
| TEST012 schema sections | runnable | M4 | `schemaDrop,getStmts` is an exact normalized EXPECTED/LOG match |
| TEST020 | blocked | M4 | INVOKE/SHOWDDL differs and the constraint matrix reaches 120 seconds |
| TEST024 | blocked | M4 | Sequence-list formatting, metadata, and error 1390 output differ |
| TEST025 | blocked | M4 | Identity SHOWDDL, sequence exhaustion, and diagnostic text differ |
| TEST027 | excluded | - | Column-family, SALT, and aligned-format physical DDL |
| TEST030 | blocked | M6 | Datetime formatting differs, including USA timestamp AM/PM output and diagnostics |
| TEST032 | blocked | M4 | INVOKE/SHOWDDL formatting and cast diagnostic 8413 differ |
| TEST033 | excluded | - | Portable grouping coverage and SALT/ESP partition coverage are inseparable in the file |
| TEST040 | excluded | - | SALT and storage-key CREATE TABLE LIKE behavior are the complete test purpose |

`TEST012` is represented by separate selectors so its portable schema sections
remain in scope without running the HBase serialization sections. No manifest
entry remains classified as `needs-review`; remaining Milestone 0 work is the
section-level treatment of entries currently marked unsafe.

## Unsafe Section Review

| TEST | Selected part | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- | --- |
| TEST002 | whole test | excluded | - | HBase flush/region/cluster statistics and Hive metadata are the test purpose |
| TEST003 | portable type sections | blocked | M6 | INVOKE formatting and TINYINT DML diagnostics differ |
| TEST003 | Hive sections | excluded | - | Hive type integration uses an external Hive shell loader |
| TEST004 | portable binary sections | blocked | M6 | SQLCI exits normally; BINARY type rendering and cast diagnostics differ |
| TEST004 | Hive section | excluded | - | Hive binary integration uses an external Hive shell loader |
| TEST026 | whole test | excluded | - | Metadata corruption cleanup directly removes HBase objects and edits physical metadata |
| TEST031 | whole test | excluded | - | The unsectioned file mixes portable cases with extensive Hive DDL/DML and an external Hive shell loader |

All former M0 entries now have a concrete roadmap milestone or an explicit
Hive/HBase exclusion. Milestone 0 has no remaining manifest entries.
