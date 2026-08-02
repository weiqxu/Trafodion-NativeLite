# Seabase Legacy Review

This file records the Milestone 0 review of the eleven remaining safe
`needs-review` entries in the `seabase` suite.

## Probe Method

Eight entries that were not wholly HBase-specific were run through the isolated
adapter with a 30 second per-test ceiling:

```sh
LOCAL_LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/localLiteLegacy/runregr --probe --suite seabase \
  TEST011 TEST012 TEST020 TEST024 TEST025 TEST030 TEST032 TEST033
```

`TEST001`, `TEST027`, and `TEST040` were excluded by source review before
execution because HBase versions/timestamps, column families, or SALT storage
are their primary purpose.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | excluded | - | HBase versions, timestamps, and physical options are the complete test purpose |
| TEST011 | unsafe | M7 | Mixed repository/HBase DML test terminates SQLCI with status 139 after the session |
| TEST012 HBase sections | excluded | - | Serialization, coprocessor, and HBase filter-pushdown coverage |
| TEST012 schema sections | blocked | M4 | Schema, INDEX, RI, VIEW, SHOWDDL, and statistics support are required |
| TEST020 | blocked | M4 | DEFAULT, CHECK, RI, ALTER, INDEX, and SHOWDDL dominate; partition cases remain mixed in |
| TEST024 | blocked | M4 | Schema, sequence lifecycle, SHOWDDL, and sequence metadata are unsupported |
| TEST025 | blocked | M4 | Identity/generated columns, schema, and SHOWDDL are unsupported; probe reached 30 seconds |
| TEST027 | excluded | - | Column-family, SALT, and aligned-format physical DDL |
| TEST030 | blocked | M6 | Datetime formatting differs, including USA timestamp AM/PM output and diagnostics |
| TEST032 | blocked | M4 | Schema, primary-key constraint changes, VIEW, ALTER, and SHOWDDL are required |
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
| TEST003 | portable type sections | blocked | M6 | TINYINT/unsigned LARGEINT/BOOLEAN coverage also needs UPDATE, DELETE, LIKE/CTAS/VIEW; the probe reached 30 seconds |
| TEST003 | Hive sections | excluded | - | Hive type integration uses an external Hive shell loader |
| TEST004 | portable binary sections | blocked | M6 | SQLCI exits normally; BINARY/VARBINARY coverage also needs LIKE/CTAS/VIEW and UPDATE/DELETE |
| TEST004 | Hive section | excluded | - | Hive binary integration uses an external Hive shell loader |
| TEST026 | whole test | excluded | - | Metadata corruption cleanup directly removes HBase objects and edits physical metadata |
| TEST031 | whole test | excluded | - | The unsectioned file mixes portable cases with extensive Hive DDL/DML and an external Hive shell loader |

All former M0 entries now have a concrete roadmap milestone or an explicit
Hive/HBase exclusion. Milestone 0 has no remaining manifest entries.
