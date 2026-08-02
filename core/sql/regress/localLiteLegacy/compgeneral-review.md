# CompGeneral Legacy Review

This file records the Milestone 0 review of the nine `compGeneral` entries that
were initially safe to probe and classified as `needs-review`. The three
remaining M0 entries containing shell or abort directives require section-level
safety review before execution.

## Probe Method

The tests were run through the isolated local-lite adapter with a 30 second
per-test ceiling:

```sh
LOCAL_LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/localLiteLegacy/runregr --probe --suite compGeneral \
  TEST001 TEST005 TEST011 TEST013 TEST015 TEST023 TEST045 TEST062 TEST071
```

RAW, LOG, normalized EXPECTED, DIFF, RESULT, SAFETY, and isolated RocksDB stores
were retained in the selected run directories. The shorter timeout makes large
optimizer tests produce a bounded baseline without treating timeout as a SQL
compatibility result.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | blocked | M2 | DELETE is unsupported; partition DDL and an ESP-dependent query also diverge |
| TEST005 | blocked | M2 | Repeated DELETE setup is unsupported; DEFAULT columns prevent several tables from being created |
| TEST011 | blocked | M2 | UPSERT/DELETE setup is unsupported; CREATE TABLE LIKE and statistics are later prerequisites |
| TEST013 | blocked | M4 | CREATE TABLE LIKE, RI constraints, and SHOWDDL are unsupported |
| TEST015 | blocked | M4 | Schema/statistics setup is required; the whole-test probe reached 30 seconds |
| TEST023 | blocked | M5 | Incremental UPDATE STATISTICS is the test purpose; UPSERT and schema DDL are prerequisites |
| TEST045 body | blocked | M4 | The portable body requires CREATE SCHEMA, statistics, and CSE EXPLAIN support |
| TEST045 cleanup | unsafe | M7 | `cleanup obsolete volatile tables` reproducibly terminated SQLCI with status 139 |
| TEST062 | blocked | M7 | SALT/partitioned storage and skew-aware executor plans are required, plus statistics and DML |
| TEST071 | blocked | M4 | Schema, DEFAULT, IDENTITY, division, ALTER, VIEW, and INDEX DDL precede extensive DML |

`TEST045` is represented by separate manifest selectors so that the portable
body can be revisited without executing the crashing cleanup section. The crash
is treated as an infrastructure failure, not as an expected semantic diff.

## Unsafe Section Review

The three remaining whole-test unsafe entries were reviewed at section level:

| TEST | Selected part | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- | --- |
| TEST004 | SQL-only body | blocked | M4 | SQLCI exits normally; CREATE SCHEMA and CREATE VIEW are the first unsupported prerequisites, followed by DML, indexes, and statistics |
| TEST004 | `ALM_6748` | unsafe | M7 | The second optimizer expression prepare reproducibly aborts SQLCI with status 134 |
| TEST004 | `LP_1324303` | blocked | M5 | The metadata catalog query and EXPLAIN cannot run against the local-lite catalog |
| TEST004 | `verify_sap_update_fix` | unsafe | M7 | The section depends on an external shell pipeline over a temporary log |
| TEST006 | SQL-only section sequence | blocked | M4 | SQLCI exits normally; VIEW, INDEX, MV, schema/storage DDL, statistics, and DML remain unsupported |
| TEST006 | unsectioned driver | unsafe | M7 | The legacy driver invokes shell cleanup for `mml.log`; the selected section sequence bypasses it |
| TEST042 | `test_ddl` | blocked | M7 | SQLCI exits normally; CREATE SCHEMA, SALT/partition constraints, statistics, and authorization initialization block hybrid query-cache setup |
| TEST042 | `test_dml` | unsafe | M7 | Query-cache validation is inseparable from shell log filtering and `/proc` inspection |

The adapter now materializes selected section bodies in manifest order. This
allows section names containing hyphens and preserves repeated cleanup sections
without executing an unsafe whole-test driver.
