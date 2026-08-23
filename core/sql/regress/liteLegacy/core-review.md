# Core Legacy Review

This file records the Milestone 0 review of the 17 `core` entries initially
classified as `needs-review`.

## Probe Method

Sixteen entries without known external directives were run through the
isolated adapter with a 30 second per-test ceiling:

```sh
LITE_LEGACY_TIMEOUT=30 \
core/sql/regress/liteLegacy/runregr --probe --suite core \
  TEST001 TEST002 TEST005 TEST008 TEST010 TEST018 TEST019 TEST020 \
  TEST027 TEST029 TEST032 TEST037 TEST041 TEST056 TEST061 TEST163
```

`TEST000` was not executed. Manual review found an external
`OBEY $$TRAF_HOME$$/sql/scripts/regrinit.sql`, so the audit and adapter safety
checks were extended to reject macro-based external OBEY paths before SQLCI.

The M2-M6 rows below were refreshed from the 2026-08-12 re-probe; every initial
timeout was retried with a 120-second limit. See
`reprobe-m2-m6-2026-08-12.tsv` for row-level status.

## Results

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST000 | unsafe | M7 | Depends on the external Trafodion `regrinit.sql` service-stack script |
| TEST001 | unsafe | M7 | SQLCI terminates with status 139 after the conditional-directive test and end of session |
| TEST002 | blocked | M2 | The scalar-subquery regression still reaches the 120-second limit |
| TEST005 | blocked | M3 | A duplicate unique-index key is rejected with local error 3242 |
| TEST008 | blocked | M7 | A complex join reaches ESP error 2013; some unordered legacy rows also differ in order |
| TEST010 | blocked | M4 | Diagnostic default-schema and message text differ |
| TEST018 | runnable | M10 | Exact normalized EXPECTED/LOG match for secondary-index DML and rollback |
| TEST019 | blocked | M7 | Hash join execution reports scratch error 8427; UPSERT, UPDATE, indexes, and statistics also remain |
| TEST020 | blocked | M7 | Forced hash-join/partition-access shapes fail to prepare with error 2105 |
| TEST027 | blocked | M4 | Control-query-shape output differs and later joins report ESP error 2013 |
| TEST029 | blocked | M4 | The 120-second probe exits normally; SHOWDDL and view-DML diagnostics differ |
| TEST032 | blocked | M6 | Unsigned key ordering differs and the conversion matrix reaches 120 seconds |
| TEST037 | blocked | M5 | INVOKE/SHOWDDL output differs and a later view reports error 4001 |
| TEST041 | blocked | M4 | Row-value CHECK ALTER requests arkcmp, reports error 2013, and reaches 120 seconds |
| TEST056 | blocked | M4 | SHOWDDL/index rendering differs and the MDAM matrix reaches 120 seconds |
| TEST061 | blocked | M5 | INVOKE/SHOWDDL differs and duplicate index creation reports error 3242 |
| TEST163 | runnable | M10 | Exact normalized EXPECTED/LOG match |

The status-139 result for `TEST001` is retained as an infrastructure failure,
not converted into a semantic diff. `TEST018` and `TEST163` are both in the
current default runnable allowlist.

## Unsafe Section Review

The remaining three `core` M0 entries were classified as follows:

| TEST | Disposition | Milestone | Observed evidence |
| --- | --- | --- | --- |
| TEST038 SQL-only sequence | blocked | M4 | Section baseline extraction remains incomplete; ESP error 2013 occurs and the sequence reaches 120 seconds |
| TEST038 `aqr` | unsafe | M7 | The AQR scenario starts a second SQLCI session from a shell command |
| TEST116 | excluded | - | The test is built around HBase object inspection, DDL transactions, concurrent HBase sessions, and region operations |
| TEST131 | unsafe | M8 | The authorization test coordinates multiple SQL users and sessions through shell-launched SQLCI processes; its native section also covers Hive privileges |

This moves all six reviewed `compGeneral` and `core` whole-test entries out of
M0 without executing their unsafe helpers.
