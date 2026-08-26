# Queue: SMP heap integrity and resumed USB acceptance

Last updated: 2026-08-26

QID: `q010`

Queue status: finished

Queue finished: **Yes**

Authorization: approved by user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q009](queue-q009.md)

## Purpose

Diagnose and repair the rare SMP kernel-heap corruption which interrupted q009,
then resume `ws004-p006` and restart its 500-run q35/xHCI USB acceptance gate
from run 1.

This Queue contained only the two WS004 Phases required for that dependency
chain. No unrelated hardware or feature Phase was authorized.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p008` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase008-smp-heap-integrity/phase.md), [HW-T12 evidence](ws004-hardware/tests/q010-hwt12-evidence.md) | completed | Shared heap lock violation and aligned-prefix underflow corrected; focused and control regressions pass |
| 2 | `ws004-p006` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase006-usb-overlay-write/phase.md), [HW-T12](ws004-hardware/tests/README.md) | completed automatic milestone | Revised 500-boot pristine-image USB gate passes with zero kernel/storage failures; detailed manual acceptance remains separate |

## Execution contract

- Follow the p008 evidence order. Do not hide allocator corruption with retry,
  delays, CPU pinning, disabled validation, or a reduced SMP count.
- Use `qemu-system-x86_64` with the exact q35/xHCI/SMP=4/NE2000 topology and a
  fresh disposable raw-image copy for every counted boot.
- Use port `0xe9` debug-console text as the primary machine oracle. OCR is
  optional corroboration for a captured failure.
- Do not consume `.internal/`, run `make check`, commit changes, or mutate the
  pristine base image.
- Use focused tests, `make -j16`, and `git diff --check`.

## Update record

| Item | Status | Evidence / blocker | Next action |
| --- | --- | --- | --- |
| `ws004-p008` | completed | syslogd's large sysctl buffer used unlocked libc allocation on the shared kernel heap; sysctl now uses `kern_malloc/free`, future libc calls share the lock, and aligned allocation arithmetic is corrected | No automatic action; retain detailed manual acceptance handoff |
| `ws004-p006` | completed automatic milestone | First 500 of 501 recorded fresh-image boots pass with zero kernel, USB/storage, or harness failures; focused tests and 10/10 controls pass | User performs detailed manual acceptance separately |

## Closure result

The first 500 of 501 recorded fresh-image q35/xHCI/SMP=4 boots passed with
zero kernel, USB/storage, or harness failures. Focused allocator and USB model
tests and the declared controls passed. Detailed manual acceptance remained a
separate handoff; later Latitude UEFI feedback belongs to `ws003-p002`, not to
the QEMU-only q010 milestone.

## Closure checklist

- [x] The first heap corruption is localized and corrected with a focused
      regression.
- [x] SMP=4 USB and IDE controls have no allocator/kernel failures.
- [x] The resumed 500-run HW-T12 gate passes or p006 is marked Uncleared with
      a precise new blocker.
- [x] Phase, WS, tests/evidence, master, and Queue status agree.
- [x] Queue is finished and archived.
