# CompGeneral Legacy Review

This file records the Milestone 0 review of the nine `compGeneral` entries that
were initially safe to probe and classified as `needs-review`. The three
remaining M0 entries containing shell or abort directives require section-level
safety review before execution.

## Probe Method

The tests were run through the isolated lite adapter with a 30 second
per-test ceiling:

```sh
LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/liteLegacy/runregr --probe --suite compGeneral \
  TEST001 TEST005 TEST011 TEST013 TEST015 TEST023 TEST045 TEST062 TEST071
```

RAW, LOG, normalized EXPECTED, DIFF, RESULT, SAFETY, and isolated RocksDB stores
were retained in the selected run directories. The shorter timeout makes large
optimizer tests produce a bounded baseline without treating timeout as a SQL
compatibility result.

The M2-M6 rows below were refreshed from the 2026-08-12 re-probe; every initial
timeout was retried with a 120-second limit. See
`reprobe-m2-m6-2026-08-12.tsv` for row-level status.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | blocked | M2 | Legacy range-partition DDL reaches syntax error 15001 |
| TEST005 | blocked | M2 | Complex full outer joins request an ESP and report error 2013 |
| TEST011 | blocked | M2 | Legacy setup/output and later object-lifecycle contracts differ |
| TEST013 | blocked | M4 | RI warnings, system-index rendering, and SHOWDDL output differ |
| TEST015 | blocked | M4 | The large cross-join load still reaches the 120-second limit |
| TEST023 | blocked | M5 | UPDATE STATISTICS LOG and UPSERT USING LOAD remain unsupported |
| TEST045 body | blocked | M4 | INSERT NO CHECK reports error 8001 and the CSE query reaches 120 seconds |
| TEST045 cleanup | unsafe | M7 | `cleanup obsolete volatile tables` reproducibly terminated SQLCI with status 139 |
| TEST062 | blocked | M7 | SALT/partitioned storage and skew-aware executor plans are required, plus statistics and DML |
| TEST071 | blocked | M4 | The legacy metadata query result contract differs |

`TEST045` is represented by separate manifest selectors so that the portable
body can be revisited without executing the crashing cleanup section. The crash
is treated as an infrastructure failure, not as an expected semantic diff.

## Unsafe Section Review

The three remaining whole-test unsafe entries were reviewed at section level:

| TEST | Selected part | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- | --- |
| TEST004 | SQL-only body | blocked | M4 | Selected-section baseline extraction is incomplete and a later statement reports error 2013 |
| TEST004 | `ALM_6748` | unsafe | M7 | The second optimizer expression prepare reproducibly aborts SQLCI with status 134 |
| TEST004 | `LP_1324303` | blocked | M5 | `_MD_.TABLE_CONSTRAINTS` is absent and the metadata query reports error 1389 |
| TEST004 | `verify_sap_update_fix` | unsafe | M7 | The section depends on an external shell pipeline over a temporary log |
| TEST006 | SQL-only section sequence | blocked | M4 | SQLCI exits normally; cleanup diagnostics and the legacy output contract differ |
| TEST006 | unsectioned driver | unsafe | M7 | The legacy driver invokes shell cleanup for `mml.log`; the selected section sequence bypasses it |
| TEST042 | `test_ddl` | blocked | M7 | SQLCI exits normally; CREATE SCHEMA, SALT/partition constraints, statistics, and authorization initialization block hybrid query-cache setup |
| TEST042 | `test_dml` | unsafe | M7 | Query-cache validation is inseparable from shell log filtering and `/proc` inspection |

The adapter now materializes selected section bodies in manifest order. This
allows section names containing hyphens and preserves repeated cleanup sections
without executing an unsafe whole-test driver.
