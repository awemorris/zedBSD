# ws001-p000: protect the inventory and gates

WSID: `ws001`

Phase ID: `p000`

Status: complete

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Establish the POSIX utility inventory, provider-state vocabulary, source/build
rules, and acceptance gates before adding commands. This Phase also protected
deferred commands from being mistaken for successful implementations.

## Design and acceptance

- Keep the machine-readable utility matrix authoritative for utility rows.
- Distinguish missing, deferred-provider, implemented-unreviewed, and reviewed.
- Require host, build/install, and QEMU evidence appropriate to each utility.
- Keep external implementations out of `userland/base`.
- Preserve correct failing tests and record unsupported behavior honestly.

Shared cases are indexed in [WS001 tests](../tests/README.md). Original detail
is retained in the [legacy plan](../history/phase000-010-legacy-plan.md).

## Result and resumption

Completed as part of the historical Phase 0–10 series. No interrupted work
remains; later inventory changes are owned directly by WS001 or a new Phase.

## Completion conditions

- The utility inventory and status vocabulary exist and are internally consistent.
- Build, provenance, host, and QEMU gate policy is documented.
- Deferred or missing commands cannot be mistaken for conforming success.
