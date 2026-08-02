# Charsets Legacy Review

This file records the Milestone 0 manual review and baseline probes for the
legacy `charsets` suite. The review covers the 14 entries that were initially
classified as `needs-review`; `TEST012` had already been classified separately.

## Probe Method

The tests were run through the isolated local-lite adapter:

```sh
core/sql/regress/localLiteLegacy/runregr \
  --probe --suite charsets --run-dir /tmp/local-lite-m0-charsets-review \
  TEST001 TEST002 TEST003 TEST004 TEST010 TEST014 \
  TEST310 TEST311 TEST312 TEST313 TEST314 TEST315 TEST316 TEST3265
```

The first run exposed two adapter compatibility issues rather than SQL
failures: lowercase self-`OBEY` filenames and lowercase `logNNN` output names.
The adapter now supplies lowercase TEST aliases in its isolated work directory
and discovers LOG files case-insensitively. Source TEST files are unchanged.

The large `TEST310` whole-test probe reached the 120 second limit. Its wrapper
and test body were reviewed separately; future convergence should run its
sections with a longer timeout or a section-aware expected baseline.

## Results

| TEST | Disposition | First roadmap dependency | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | blocked | M4 | ALTER TABLE CHECK is unsupported; UCS2 hexadecimal assignments also diverge in M6 |
| TEST002 | blocked | M4 | DEFAULT, CHECK, RI, VIEW, and INDEX DDL are unsupported |
| TEST003 | blocked | M2 | UPDATE now succeeds; DELETE is the next unsupported DML operation, with catalog metadata and character-set differences later |
| TEST004 | blocked | M6 | UCS2 `CHAR` assignment reports error 8690 |
| TEST010 | blocked | M2 | DELETE is unsupported and UCS2 parameter results diverge |
| TEST014 | blocked | M4 | DEFAULT and VIEW/schema DDL fail before INTERVAL/UCS2 convergence |
| TEST310 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 implicit-cast coverage |
| TEST311 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 volatile-table cast coverage |
| TEST312 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 UTF8 cast coverage |
| TEST313 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 UTF8/UCS2 cast coverage |
| TEST314 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 two-byte UTF8 coverage |
| TEST315 | blocked | M4 | CREATE SCHEMA wrapper is unsupported; body is M6 three-byte UTF8 coverage |
| TEST316 | runnable | M10 | Exact normalized EXPECTED/LOG match |
| TEST3265 | blocked | M6 | 64K UTF8 value assignment reports error 8690 |

Legacy negative tests also expose missing diagnostic message text as additional
`ERROR[16001]` lines. These differences are retained; the adapter does not hide
them with a broad filter. They must be addressed deliberately during suite
convergence without discarding the primary SQL error codes.
