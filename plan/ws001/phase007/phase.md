# ws001-p007: compression utilities

WSID: `ws001`

Phase ID: `p007`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Add the planned compression/decompression utilities with local source,
round-trip behavior, malformed-stream handling, and package installation.

## Design and acceptance

- Bound dictionary/table/input sizes and reject corrupt streams safely.
- Verify deterministic round trips and documented format compatibility.
- Run host fixtures, Phase 7 QEMU tests, standalone installs, and top build.
- Leave unsupported format variants as explicit ledger items.

Cases and fixtures are indexed in [WS001 tests](../tests/README.md).

## Result and resumption

The implementation milestone completed without claiming every historical
format/option combination reviewed.

## Completion conditions

- Selected compression and decompression commands build and install locally.
- Round-trip, corrupt-stream, bounds, and documented interoperability cases pass.
- Phase 7 host, build, and QEMU gates pass.
