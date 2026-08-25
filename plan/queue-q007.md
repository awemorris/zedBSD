# Queue: stable QEMU USB boot/root

Last updated: 2026-08-25

QID: `q007`

Queue status: finished

Queue result: USB-root identity completed; correctness bugs uncleared

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q006](queue-q006.md)

Next Queue: [q008](queue.md)

## Purpose

Execute HW-02 through QEMU U0--U5: boot the amd64 PC/AT image from xHCI USB
mass storage and retain the firmware-selected boot medium independently of
native device discovery order.

## Execution registry

| Priority | Item | WS / Phase | Sources | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | USB boot/root continuity | `ws004-p005` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase005-usb-root/phase.md), [tests](ws004-hardware/tests/README.md) | uncleared | BIOS/UEFI identity and bounded discovery pass, but USB overlay writes and clean warm reboot do not |

## Closure result

Stable FAT UUID selection, BIOS/UEFI USB boot, reordered-device selection,
duplicate rejection, and bounded delayed/missing media were completed. The
acceptance result was reopened when ordinary root login reproduced `loop1`
write `EIO` on the USB-backed `DATA.IMG`. Clean warm reboot also retains
allocator BSS state on the second kernel entry.

The unresolved defects are split into:

- [`ws004-p006`](ws004-hardware/phase006-usb-overlay-write/phase.md) for USB
  loop-backed overlay writes; and
- [`ws004-p007`](ws004-hardware/phase007-warm-reset/phase.md) for PC/AT warm
  reset and BSS reinitialization.

## Closure checklist

- [x] Stable boot-selector implementation and non-write QEMU cases recorded.
- [x] The contradicted USB-write acceptance claim is marked uncleared.
- [x] Both correctness defects have bounded follow-up Phases.
- [x] Uncleared work is carried into `q008` rather than marked `completed`.
