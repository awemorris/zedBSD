# ws001-p001: temporary failure commands

WSID: `ws001`

Phase ID: `p001`

Status: complete

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Install explicit commands for utilities whose required provider/service was not
yet available. The commands had to fail clearly and nonzero rather than appear
implemented or silently succeed.

## Design and acceptance

- Share one local implementation pattern without importing external code.
- Emit a stable diagnostic naming the unavailable provider.
- Reject unsupported options/operands and return failure.
- Exercise staged rootfs and QEMU command behavior.

Test ownership is recorded in [WS001 tests](../tests/README.md), including the
deferred-stub host/rootfs/QEMU cases.

## Result and resumption

The temporary failure behavior was completed. Each stub remains compliance debt
until a later provider Phase replaces it and updates the WS001 ledger.

## Completion conditions

- Every selected provider-missing command installs in the staged image.
- Each command emits the documented diagnostic and exits nonzero.
- Host, rootfs, and QEMU deferred-stub cases pass.
