# Queue: USB URB completion correctness

Last updated: 2026-08-26

QID: `q009`

Queue status: finished

Queue finished: **Yes**

Authorization: approved by user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q008](queue-q008.md)

## Purpose

Execute only `ws004-p006`: diagnose and repair the intermittent q35/xHCI USB
overlay write failure, then verify the correction against the Phase's focused
and repeated-boot gates.

No other Phase was authorized in this Queue.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p006` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase006-usb-overlay-write/phase.md), [HW-T12](ws004-hardware/tests/README.md) | uncleared | URB publication/lifecycle correction passes focused checks; the 1,000-boot gate is blocked by a separately planned SMP heap fault |

## Execution contract

- Follow the hypothesis and stop rules in the Phase Book; do not expand into
  unrelated xHCI features, USB HID, physical Latitude acceptance, or another
  Phase.
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
| `ws004-p006` | uncleared | H1 is proved and corrected with release/acquire publication plus single terminal ownership. Focused tests and build pass. HW-T12 produced 35 clean boots, while run 26 faulted in kernel `remove_free()`; the retained image then rebooted cleanly. | Transferred to q010 after its prerequisite `ws004-p008` heap diagnosis |

## Closure checklist

- [x] H1 is proved or rejected with correlated evidence.
- [x] The owning correction and focused lifecycle regressions pass.
- [x] The pristine-image repeated-boot gate and persistence/control cases pass,
      or the Phase is marked Uncleared with a precise resume condition.
- [x] Phase, WS, tests/evidence, master, and Queue status agree.
- [x] Queue is finished and archived before q010 replaces it.
