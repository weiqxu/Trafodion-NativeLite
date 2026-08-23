# Lite M20: prepared plans and server-side SQL batching

## Scope

M20 removes the M19 protocol-only parameter template from the normal T4
`Prepare`/`Execute` path. A T4 statement now owns a session-local CLI plan,
its input descriptor metadata, and a reusable input buffer. Execute decodes
the wire rowset and binds null indicators and typed values server-side.

The reduced lite compiler does not expose a key-aware executor plan for
parameter markers in predicates. For parameterized `SELECT`, `UPDATE`, and
`DELETE` statements with a `WHERE` clause, M20 retains the prepared metadata
and binding contract but specializes the bound SQL at execution so the
compiler can recover the normal primary/index lookup. INSERTs and statements
without keyed predicates continue to use the retained CLI plan directly.

DECIMAL, DATETIME, and other descriptors that cannot safely be populated from
the compact T4 text row are prepared as `CAST(? AS VARCHAR(256))`. Integer,
floating point, boolean, fixed/variable character, and NULL values use the
native CLI descriptor layout when the retained plan executes directly.

## Batch execution

* A T4 INSERT rowset executes each row against the prepared statement inside
  one server request and one transaction boundary.
* ExecuteDirect and the compatibility Execute path accept multiple top-level
  semicolon-separated statements. The server splits outside quoted strings
  and runs the statements sequentially before returning the final result.
* Multi-statement prepared text keeps the existing protocol parameter layout
  and is handled by the server batch splitter; ordinary single statements use
  the session-local prepared plan unless they contain a keyed lite
  predicate requiring specialization.
* TPCC New-Order now binds its district/order/new-order/stock/order-line
  mutation phase as one multi-statement batch after the read snapshot phase.

## CLI lifecycle constraint

Trafodion's current CLI close path retains executor state that can reuse the
previous input row for a subsequent dynamic execution. M20 clears stale input
copies in `Statement::execute`, rebinds descriptor pointers after cursor close,
and reopens the retained plan when the CLI requires a fresh cursor. The plan
and descriptor ownership remain server-side; keyed lite predicates use
the documented specialization fallback because the current compiler cannot
bind a parameter marker into its key access metadata.

## Validation

* `make lite -j2`
* `scripts/test-lite-t4jdbc.sh`
* `TPCC_SCALE=smoke` with a one-warehouse/one-terminal qualification profile:
  load, workload, online checkpoint, clean/unclean restart, checkpoint
  restore, and disk-watermark gates pass; the one-terminal run completed with
  zero unclassified errors and OCC client retries handled normally. The
  measured smoke baseline was 1.005 TPS (20 measured transactions/terminal),
  not a production performance claim.
* The default two-warehouse/two-terminal qualification profile passed after
  keyed literal specialization: 1.819 TPS, 0 client OCC retries, zero server
  validation conflicts, 47 full scans, and 110 primary-range reads. This is
  still a reduced TPC-C-like gate, not a production or formal TPC-C claim.

The multi-warehouse run now passes the concurrency gate, while the remaining
work is to replace keyed literal specialization with a compiler/runtime plan
that can bind parameter markers directly without losing primary-key access.
