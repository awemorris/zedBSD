# Queue: ECAM and MSI interrupt ownership

Last updated: 2026-08-25

QID: `q004`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q003](queue-q003.md)

## Purpose and status

Implement the architecture decision that unblocked PCI ECAM and MSI work. This
Queue contains one cohesive Phase because its HAL, ACPI, vector-allocation, PCI,
and runtime evidence must agree as one lifecycle.

## Execution registry

| Priority | Item | WS / Phase | Sources | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | ECAM and MSI interrupt ownership | `ws004-p003` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase003-ecam-msi/phase.md), [tests](ws004-hardware/tests/README.md) | completed | Software milestone: fixed HAL API, validated MCFG/ECAM, amd64 dynamic logical IRQs, PCI MSI/MSI-X lifecycle, focused tests, portable unsupported boundary, and real QEMU delivery evidence |

## Execution policy

- Do not commit; do not use `make check` or `.internal/`.
- Preserve unrelated and earlier-Queue worktree changes.
- Keep focused tests under `plan/ws004-hardware/tests/`.
- Use `make -j16` for integration and `qemu-system-x86_64` for runtime evidence.
- Carry forward a bounded hardware-fixture limitation, but return to human
  judgment for any reconsideration trigger listed by the Phase.

## Update record

| Item | Status | Evidence / blocker | Next action |
| --- | --- | --- | --- |
| 1 | completed | Five focused host fixtures, amd64/PCAT/PC-98 links, strict unsupported-port syntax checks, `make -j16`, and q35 EDU MSI delivery through `login:` pass. Non-installed cross toolchains prevent full arm64/sparcv9/m68k links. | Extract xHCI work separately; retain the bounded cross-link environment handoff |

## Closure checklist

- [x] The item is completed or uncleared with evidence.
- [x] Focused tests and configured architecture builds are recorded.
- [x] Required QEMU evidence or its bounded limitation is recorded.
- [x] Phase, WS, and master status rows agree.
- [x] `Queue status` is finished and `Queue finished` is **Yes**.
