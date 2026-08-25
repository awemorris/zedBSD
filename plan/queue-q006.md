# Queue: QEMU xHCI host controller

Last updated: 2026-08-25

QID: `q006`

Queue status: finished

Queue result: QEMU storage milestone completed

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q005](queue-q005.md)

## Purpose

Execute the next dependency-ready master-plan item: native xHCI sufficient for
QEMU USB mass-storage enumeration and transfer lifecycle. USB-root selection is
kept in the following HW-02 Phase.

## Execution registry

| Priority | Item | WS / Phase | Sources | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | xHCI host controller | `ws004-p004` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase004-xhci/phase.md), [tests](ws004-hardware/tests/README.md) | completed | QEMU storage milestone: production MSI-X xHCI storage enumeration, media read, and reconnect pass |

## Update record

| Item | Status | Evidence / blocker | Next action |
| --- | --- | --- | --- |
| 1 | completed | Approved USB lifecycle is implemented; q35 MSI-X enumeration, 4096-byte media read, login, disconnect, and reconnect pass | Extract USB-root continuity separately; retain SuperSpeed/fault injection and Latitude evidence |

## Closure checklist

- [x] Phase extracted and linked.
- [x] Production xHCI driver is registered and built.
- [x] Focused/build/QEMU acceptance passes or a concrete human decision is recorded.
- [x] Queue is archived before the next extraction.
