# Queue: USB correctness bug fixes

Last updated: 2026-08-25

QID: `q008`

Queue status: in-progress

Queue finished: **No**

Authorization: approved by user on 2026-08-25

Timebox: this session, no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q007](queue-q007.md)

## Purpose

Prioritize reproducible correctness defects in the amd64 USB path before
extracting new feature work. This Queue first repairs the Latitude UEFI loader
failure, then restores reliable writes to the QEMU USB-backed root overlay and
repairs the independent warm-reset/BSS path.

Feature expansion, USB HID, NVMe, and previously non-reproducible X11 pointer
work remain outside this Queue.

## Status model

| State | Meaning |
| --- | --- |
| pending | Selected for this Queue but not started; execution approval is still required |
| in-progress | Diagnosis, implementation, or verification is active |
| completed | Every local Phase completion condition has evidence |
| uncleared | Work was attempted but remains uncleared; evidence and a concrete resume condition are recorded |

`Queue finished` becomes **Yes** when every item is completed or uncleared,
all owning Phase/WS/master records agree, and no item remains pending or
in-progress. A finished Queue may retain explicitly uncleared defects.

## Execution registry

| Priority | Bug | WS / Phase | Authoritative documents | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | Latitude USB UEFI loader halts around final memory-map/boot-services exit | `ws003-p002` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase002-uefi-memory-map/phase.md), [WS003 tests](ws003-bringup/tests/README.md) | in-progress | Final GetMemoryMap is consumed immediately by ExitBootServices; QEMU remains working and three Latitude cold boots reach kernel entry |
| 2 | USB `loop1` overlay writes return `EIO` during ordinary root login | `ws004-p006` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase006-usb-overlay-write/phase.md), [HW-T12](ws004-hardware/tests/README.md) | in-progress | Reproduce the block-32/block-40 EIO under q35, xHCI, SMP=4, and NE2000; identify and fix the first failing boundary |
| 3 | Warm reboot enters the second kernel with stale allocator BSS state | `ws004-p007` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase007-warm-reset/phase.md), [HW-T13](ws004-hardware/tests/README.md) | completed | Native-mode ELF64 BSS clearing passes three consecutive IDE reboots and a combined USB reboot |

## Execution order

1. Run `ws003-p002` first because it is the earliest physical boot failure and
   blocks kernel entry on the target. Preserve QEMU/OVMF as the control.
2. Run `ws004-p006` next. The defect is reproducible during normal QEMU USB
   boot and invalidates writable-root acceptance.
3. Run `ws004-p007` only after USB writes are cleared, so filesystem I/O does
   not contaminate reboot diagnosis. IDE may be used as a control, but USB must
   pass the final combined path.
4. If a diagnosis proves that a broader block, VFS, loader, SMP, or HAL
   redesign is required, stop that item and record the evidence instead of
   forcing an unsafe patch.

## Excluded known work

- USB HID is planned feature work (`WS006 IN-10/IN-11`), not a regression.
- The X11 pointer defect remains carried forward because the original mismatch
  was not reproduced and no new reproducer is available.
- USB write throughput is performance debt; correctness and persistence take
  priority in this Queue.

## Verification policy

- Use disposable copies of disk images for every write/failure experiment.
- Use `qemu-system-x86_64` and record complete device/drive arguments.
- Do not use `.internal/` test material or the repository-wide `make check`.
- Use focused WS003/WS004 tests, `make -j16`, and `git diff --check`.
- Do not commit changes.

## Update record

| Item | Status | Evidence / blocker | Next action |
| --- | --- | --- | --- |
| 1 | in-progress | Latest Latitude run stops visibly at `A64 UEFI READY`; final normalization was moved after ExitBootServices and the corrected OVMF USB path reaches login | Run the corrected image on Latitude three times and record whether kernel entry follows READY |
| 2 | in-progress | A newly generated image reproduces on its first QEMU boot only; diagnostics show a 31-byte BOT CBW completing with `actual=0`, before SCSI WRITE(10) | Re-run the supplied GUI command against the xHCI ring-wrap/event-ownership correction and report any CBW/loop error |
| 3 | completed | ELF64 BSS is cleared in native mode immediately before kernel entry; IDE reboot x3 and USB reboot x1 reach login | No local action; physical reset behavior can be evaluated in a future hardware Queue |

## Closure checklist

- [ ] Every item is completed or uncleared with a precise resume condition.
- [ ] `ws004-p005` and HW-T11 reflect the final USB-root acceptance result.
- [ ] Parent WS and master rows reflect the final status.
- [x] No passing claim relies only on a usable shell while storage errors are
      present in the console log.
- [ ] Queue is finished and ready to archive before a later Queue replaces it.
