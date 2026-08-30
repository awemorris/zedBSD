# Queue: PC-98 IPL entry localization and Xzed mouse recovery

Last updated: 2026-08-31

QID: `q039`

Queue status: in-progress

Queue finished: **No**

Authorization: the user reported that the PC-9821V13 still does not boot,
asked whether the IPL1 signature is correct, and supplied a reproducible PC-98
Xzed mouse regression. This Queue performs the bounded diagnosis and repair
requested by that report.

Timebox: none. Automatic work continues through both items. The Queue does not
wait on the physical V13 checkpoint before repairing and accepting the
QEMU-reproducible mouse defect.

Parent: [master plan](master.md)

Previous Queue: [q038](queue-q038.md)

## Purpose

Prove the installed PC-98 IPL signature instead of changing it speculatively,
then prepare one immutable audio-trace image which localizes the physical
Stage-1/Stage-2 boundary in one boot. Independently repair the demonstrated
PC-98 slave-PIC cascade defect which prevents Xzed from receiving bus-mouse
events, and retain both a focused PIC contract and a production QEMU cursor
movement regression.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p023` | [Phase](ws003-bringup/phase023-pc9821-v13-ipl-entry-localization/phase.md) | uncleared | Automatic signature, corruption, normal-login, and diagnostic-login gates pass; one exact hashed image awaits one V13 audio observation |
| 2 | `ws007-p003` | [Phase](ws007-graphics/phase003-pc98-xzed-mouse-pic-cascade/phase.md) | in-progress | Slave IRQ lifecycle preserves PC-98 master IRQ7 cascade and QEMU moves the Xzed cursor by the injected relative delta |

## Fixed boundaries

- Keep the native PC-98 LBA0/LBA1/LBA2 format. Do not add a PC/AT MBR or
  remove the trailing `55 aa`.
- `IPL1` at LBA0 offsets 4--7 is already the accepted contract. Do not treat
  bytes 508--509 as a Stage-1 sector count; they are the PC-98 boot-menu
  version/reserved bytes.
- Audio tracing is diagnostic-only and must not add a beep to the ordinary
  production image.
- Do not claim the physical V13 issue fixed until the single identified
  diagnostic image has been observed on that machine.
- The mouse repair belongs to the PC-98 PIC cascade. Do not bypass PIC
  delivery in Xzed, evdev, or the bus-mouse driver.
- Do not consume `.internal/` or run aggregate `make check`.
- Use `make -j16`, focused host gates, the production image checker, and the
  maintained qemu-pc98 binary.
- Commit and push after each Phase. If push is unavailable, retain the local
  commit and continue.

## Completion definition

q039 is finished when each selected item is either completed or uncleared with
its exact evidence and resume condition recorded. `ws003-p023` may be
uncleared solely at its one external PC-9821V13 observation while
`ws007-p003` completes automatically.
