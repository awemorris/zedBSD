# Queue: USB correctness bug fixes

Last updated: 2026-08-26

QID: `q008`

Queue status: finished

Queue finished: **Yes**

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
| 1 | Latitude USB UEFI loader halts around final memory-map/boot-services exit | `ws003-p002` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase002-uefi-memory-map/phase.md), [WS003 tests](ws003-bringup/tests/README.md) | uncleared | Software sequencing correction and OVMF control pass; three Latitude cold boots remain human-operated acceptance work |
| 2 | USB `loop1` overlay writes intermittently return `EIO` during startup/login | `ws004-p006` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase006-usb-overlay-write/phase.md), [HW-T12](ws004-hardware/tests/README.md) | uncleared | Leading URB publication-race hypothesis and 1,000-boot gate are carried into `q009` as its sole item |
| 3 | Warm reboot enters the second kernel with stale allocator BSS state | `ws004-p007` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase007-warm-reset/phase.md), [HW-T13](ws004-hardware/tests/README.md) | completed | Native-mode ELF64 BSS clearing passes three consecutive IDE reboots and a combined USB reboot |

## Execution order

1. Run `ws003-p002` first because it is the earliest physical boot failure and
   blocks kernel entry on the target. Preserve QEMU/OVMF as the control.
2. Run `ws004-p006` next. The defect is timing-dependent during normal QEMU USB
   boot and invalidates writable-root acceptance. Follow the Phase hypothesis
   order; do not treat the earlier xHCI ring correction as a demonstrated fix.
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
- Use port `0xe9` debug-console text as the primary machine-readable oracle for
  repeated boots. Screen capture/OCR is corroborating evidence, not the pass
  oracle, because the amd64 console already mirrors the same characters to the
  VGA display and debug port.
- Do not use `.internal/` test material or the repository-wide `make check`.
- Use focused WS003/WS004 tests, `make -j16`, and `git diff --check`.
- Do not commit changes.

## Update record

| Item | Status | Evidence / blocker | Next action |
| --- | --- | --- | --- |
| 1 | uncleared | Latest Latitude run stops visibly at `A64 UEFI READY`; final normalization was moved after ExitBootServices and the corrected OVMF USB path reaches login | Resume only in a later Queue with physical Latitude access |
| 2 | uncleared | User reruns after the xHCI ring correction still fail at varying times: CBW, 4,096-byte Bulk OUT data, and a valid CSW are each observed as `error=0 actual=0`. The CSW tag/status prove input data was copied, while the caller still read zero length. The current amd64 object stores terminal URB status before `actual_length`, exposing an SMP publication race. | Transferred unchanged to `q009` for focused execution |
| 3 | completed | ELF64 BSS is cleared in native mode immediately before kernel entry; IDE reboot x3 and USB reboot x1 reach login | No local action; physical reset behavior can be evaluated in a future hardware Queue |

## Closure checklist

- [x] Every item is completed or uncleared with a precise resume condition.
- [ ] `ws004-p005` and HW-T11 reflect the final USB-root acceptance result.
- [ ] Parent WS and master rows reflect the final status.
- [x] No passing claim relies only on a usable shell while storage errors are
      present in the console log.
- [x] Queue is finished and archived before `q009` replaces it.
