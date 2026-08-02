# Core Legacy Review

This file records the Milestone 0 review of the 17 `core` entries initially
classified as `needs-review`.

## Probe Method

Sixteen entries without known external directives were run through the
isolated adapter with a 30 second per-test ceiling:

```sh
LOCAL_LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/localLiteLegacy/runregr --probe --suite core \
  TEST001 TEST002 TEST005 TEST008 TEST010 TEST018 TEST019 TEST020 \
  TEST027 TEST029 TEST032 TEST037 TEST041 TEST056 TEST061 TEST163
```

`TEST000` was not executed. Manual review found an external
`OBEY $$TRAF_HOME$$/sql/scripts/regrinit.sql`, so the audit and adapter safety
checks were extended to reject macro-based external OBEY paths before SQLCI.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST000 | unsafe | M7 | Depends on the external Trafodion `regrinit.sql` service-stack script |
| TEST001 | unsafe | M7 | SQLCI terminates with status 139 after the conditional-directive test and end of session |
| TEST002 | blocked | M2 | DELETE is a prerequisite; diagnostic differences remain and the whole test reached 30 seconds |
| TEST005 | blocked | M3 | Secondary indexes are unsupported; INTERVAL, DELETE, and statistics also diverge |
| TEST008 | blocked | M7 | A complex join reaches ESP error 2013; some unordered legacy rows also differ in order |
| TEST010 | blocked | M4 | CREATE VIEW is unsupported; executor statistics and UCS2 are later dependencies |
| TEST018 | blocked | M3 | Index maintenance requires secondary indexes plus UPDATE and DELETE |
| TEST019 | blocked | M7 | Hash join execution reports scratch error 8427; UPSERT, UPDATE, indexes, and statistics also remain |
| TEST020 | blocked | M7 | Forced hash-join/partition-access shapes fail to prepare with error 2105 |
| TEST027 | blocked | M4 | CREATE VIEW is required before INTERVAL and ESP-dependent join coverage |
| TEST029 | blocked | M4 | DEFAULT, CHECK, VIEW, SHOWDDL, ALTER, and view DML are unsupported |
| TEST032 | blocked | M6 | Unsigned numeric conversion and key ordering diverge; the whole test reached 30 seconds |
| TEST037 | blocked | M5 | INVOKE requires catalog metadata; VIEW and additional types follow |
| TEST041 | blocked | M4 | CHECK constraints and ALTER TABLE are unsupported; the whole test reached 30 seconds |
| TEST056 | blocked | M4 | DEFAULT/ALTER/VIEW and SHOWDDL are required; the whole test reached 30 seconds |
| TEST061 | blocked | M5 | INVOKE/SHOWDDL metadata is required before VIEW, INDEX, and UCS2 coverage |
| TEST163 | runnable | M10 | Exact normalized EXPECTED/LOG match |

The status-139 result for `TEST001` is retained as an infrastructure failure,
not converted into a semantic diff. `TEST163` joins `charsets/TEST316` in the
default runnable allowlist.

## Unsafe Section Review

The remaining three `core` M0 entries were classified as follows:

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST038 SQL-only sequence | blocked | M4 | The isolated probe reached the 30 second ceiling while still executing the expression/DML body; DEFAULT, CTAS, SHOWDDL, UPDATE, DELETE, statistics, and additional types remain dependencies |
| TEST038 `aqr` | unsafe | M7 | The AQR scenario starts a second SQLCI session from a shell command |
| TEST116 | excluded | - | The test is built around HBase object inspection, DDL transactions, concurrent HBase sessions, and region operations |
| TEST131 | unsafe | M8 | The authorization test coordinates multiple SQL users and sessions through shell-launched SQLCI processes; its native section also covers Hive privileges |

This moves all six reviewed `compGeneral` and `core` whole-test entries out of
M0 without executing their unsafe helpers.
