# ws001-p005: process, credentials, and System V IPC tools

WSID: `ws001`

Phase ID: `p005`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Implement the planned process, credential, priority, and System V IPC utilities
against explicit kernel/UAPI behavior rather than host-only substitutes.

## Design and acceptance

- Define kernel/UAPI ownership and permission checks before command behavior.
- Keep parsing/formatting host-testable but validate actual syscalls in QEMU.
- Cover invalid IDs, permissions, ranges, and resource cleanup.
- Run Phase 5 focused host cases, build/install gates, and QEMU integration.

Shared cases are indexed in [WS001 tests](../tests/README.md).

## Result and resumption

The planned Phase completed. Any incomplete syscall semantics remain explicit
kernel/API rows in [WS001](../ws.md), not hidden by utility success.

## Completion conditions

- Selected process, credential, priority, and IPC commands build and install.
- Required kernel/UAPI permission, invalid-ID, range, and cleanup cases pass.
- Host logic, top-level build, and Phase 5 QEMU tests pass.
