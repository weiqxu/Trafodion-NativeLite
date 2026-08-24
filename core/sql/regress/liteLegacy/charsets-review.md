# Charsets Legacy Review

This file records the Milestone 0 manual review and the current baseline probes
for all 15 legacy `charsets` inputs. The initial review covered 14 entries that
were classified as `needs-review`; `TEST012` was classified separately.

## Probe Method

The tests were run through the isolated Trafodion Lite adapter:

```sh
core/sql/regress/liteLegacy/runregr \
  --probe --suite charsets --run-dir /tmp/lite-m0-charsets-review \
  TEST001 TEST002 TEST003 TEST004 TEST010 TEST014 \
  TEST310 TEST311 TEST312 TEST313 TEST314 TEST315 TEST316 TEST3265
```

The first run exposed two adapter compatibility issues rather than SQL
failures: lowercase self-`OBEY` filenames and lowercase `logNNN` output names.
The adapter now supplies lowercase TEST aliases in its isolated work directory
and discovers LOG files case-insensitively. Source TEST files are unchanged.

All M2-M6 blocked entries were re-probed on 2026-08-12. Initial 30-second
timeouts were rerun at 120 seconds. `TEST310`-`TEST313` still reached that limit;
their current progress and output differences are retained in
`reprobe-m2-m6-2026-08-12.tsv`.

## Results

| TEST | Disposition | First roadmap dependency | Observed evidence |
| --- | --- | --- | --- |
| TEST001 | blocked | M4 | SQLCI aborts in `fbstring` after rendering two UCS2 sort-key rows |
| TEST002 | blocked | M4 | Triggers now create; legacy SHOWDDL and later output contracts differ |
| TEST003 | runnable | M10 | Exact normalized EXPECTED/LOG match |
| TEST004 | runnable | M6 | Exact normalized EXPECTED/LOG match after the M2-M6 re-probe |
| TEST010 | runnable | M2 | Exact normalized EXPECTED/LOG match after the M2-M6 re-probe |
| TEST012 | blocked | M4 | INVOKE/SHOWDDL and UCS2 default-literal rendering differ |
| TEST014 | blocked | M4 | Legacy HASH2 PARTITION BY is rejected with error 1199 |
| TEST310 | blocked | M4 | Reaches the 120-second limit in the TRIM predicate sequence |
| TEST311 | blocked | M4 | Reaches the 120-second limit in the RTRIM predicate sequence |
| TEST312 | blocked | M4 | UTF8 output widths differ and the DECODE query sequence times out |
| TEST313 | blocked | M4 | UCS2 output widths differ and the string query sequence times out |
| TEST314 | runnable | M4 | Exact normalized EXPECTED/LOG match after the M2-M6 re-probe |
| TEST315 | blocked | M4 | UTF8 TRIM length, padding, and row order differ |
| TEST316 | runnable | M10 | Exact normalized EXPECTED/LOG match |
| TEST3265 | blocked | M6 | 64K VARCHAR statistics reaches an unsupported GET metadata request |

A clean `lite` build now installs the diagnostic message catalog, so the
runnable baseline records the primary SQL error codes with their resolved
messages and no longer expects secondary `ERROR[16001]` lines. The adapter still
does not hide diagnostic differences with a broad filter.
