# NativeLite TPC-C Qualification Assets

This directory contains the versioned inputs for the NativeLite M14
qualification workload. The normative reference is TPC-C 5.11.0, available
from <https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf>.
The specification itself is not vendored.

These assets define a dialect-adapted, non-audited workload. Passing repository
gates permits the label `TPC-C-like`; it does not permit an official TPC-C or
`tpmC` claim.

## Versioned contract

- `qualification.properties` fixes the qualification scale, seeds, batching,
  terminal, retry, and reporting contract.
- `schema-manifest.tsv` maps the nine logical TPC-C entities to NativeLite table
  names and declares their keys and cardinalities.
- `dialect-deviations.tsv` records every known departure from the normative
  schema or benchmark procedure.

The repository-owned JDBC workload driver is implemented under `scripts/` and
uses the reduced NativeLite T4 endpoint. It is intentionally inspectable and is
not represented as a TPC-certified driver.

Run `make local-lite-m14a` to validate these inputs and emit a machine-readable
baseline report. Run `make local-lite-m14b` for the fast smoke-scale schema,
loader, interruption/retry, restart, and backup/restore gate. Set
`TPCC_M14_SCALE=qualification` to exercise the pinned one-warehouse scale:

```sh
TPCC_M14_SCALE=qualification make local-lite-m14b
```

The loader is deterministic and restartable at bounded commit boundaries. It
uses the real reduced T4 JDBC endpoint; its report contains the exact generated
cardinalities and post-load consistency result.
