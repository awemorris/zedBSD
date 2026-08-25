# WS004 Phase 006: USB loop-backed overlay write reliability

Last updated: 2026-08-25

Phase ID: `ws004-p006`

Status: in progress

Acceptance disposition: **Uncleared**

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Eliminate the reproducible `loop1` write `EIO` when the production amd64 image
boots as q35 xHCI USB mass storage, and prove that the writable `DATA.IMG`
overlay survives ordinary login activity, explicit file I/O, synchronization,
and a cold restart.

## Baseline defect

Manual acceptance on 2026-08-25 reproduced the defect by booting the normal
image from USB. Immediately after root login, the guest reported:

```text
loop1: write block=48 count=8 flags=2 error=5
loop1: write block=56 count=8 flags=2 error=5
```

`loop1` is the read-write UFS data image used as the overlay upper. `error=5`
is `EIO`; a usable prompt and successful reads do not clear the failure. This
supersedes the earlier retained-checksum claim in `ws004-p005`: USB-root write
acceptance is Uncleared until this Phase passes.

## Scope

- Reproduce on a disposable copy of the production amd64 image with the exact
  q35/xHCI/USB-storage command recorded.
- Run the same image through IDE as the control case.
- Trace the first failing boundary across overlay/UFS, `loop1`, FAT backing-file
  writes to `DATA.IMG`, USB Mass Storage BOT/SCSI, and xHCI bulk completion.
- Add diagnostics only where they preserve the original error and identify the
  failing LBA, transfer length, command/status, and completion code.
- Correct the owning layer without hiding an error, retrying indefinitely, or
  weakening filesystem consistency checks.
- Verify write, flush/sync, readback, cold persistence, and subsequent boot
  consistency on disposable images.

## Non-goals

- General USB write-performance optimization.
- Physical Latitude completion or unrelated USB class drivers.
- Redesigning the overlay layout or replacing `DATA.IMG` merely to bypass the
  faulty path.
- Repairing the independent PC/AT warm-reset/BSS defect; that is `ws004-p007`.

## Expected files and subsystems

- `drivers/usb-storage.c`, `drivers/pci-xhci.c`, and `drivers/usb.c`
- `drivers/loop.c`, `src/kern/fat-vfs.c`, and the block/buffer path as indicated
  by reproduction evidence
- WS004 QEMU evidence and focused USB-storage/xHCI fixtures

The implementation must follow the evidence. Files outside this list are
changed only when the first failing boundary proves ownership there.

## Ordered work packages

- [x] Capture a minimal reproducible USB boot and an IDE control run.
- [x] Determine whether the first error originates above BOT, in BOT/SCSI, in
      xHCI transfer handling, or at the emulated device boundary.
- [x] Add a focused SCSI-response regression for the corrected boundary.
- [x] Implement the smallest correctness fix at the owning layers.
- [x] Run login-triggered writes and an explicit bounded write/readback
      workload without any `loop1`, UFS, FAT, BOT, or xHCI error.
- [x] Cold-boot the same disposable image and verify retained contents and
      filesystem consistency.
- [x] Run the IDE control, focused SCSI fixture, configured amd64 build,
      `make -j16`, and `git diff --check`; do not use `make check`.
- [x] Update `ws004-p005` and HW-T11 after the write gates pass.
- [ ] Re-run a newly generated image with the user's exact GUI command and
      verify that the initial login path has no CBW, xHCI, or loop error.

## Completion conditions

- Three fresh USB boots reach login without a `loop1` write error during normal
  startup/login activity.
- A bounded multi-block create/write/fsync/readback workload passes and its
  contents survive a QEMU cold restart of the same disposable image.
- The test crosses the `DATA.IMG` loop/FAT backing path; a tmpfs-only write does
  not count.
- IDE attachment of the same image remains correct.
- Failure injection or read-only media still returns a visible bounded error;
  the fix must not report false success.
- Focused tests, relevant configured builds, `make -j16`, and
  `git diff --check` pass. The repository-wide `make check` target is not used.

## Reopened acceptance finding

User acceptance on 2026-08-25 reproduced the writable-overlay failure with a
newly generated image on its first production-style q35/xHCI boot, using four
virtual CPUs and an ISA NE2000 device. The same image does not reproduce on its
second boot:

```text
loop1: write block=32 count=8 flags=2 error=5
loop1: write block=40 count=8 flags=2 error=5
```

This invalidates the earlier QEMU clearance. The three passing runs were an
insufficient sample and the read-only-media propagation fix did not address
this writable-media defect. Diagnosis resumes with this exact topology; the
Phase remains Uncleared until repeated acceptance passes without any storage
error.

Additional BOT diagnostics narrowed the first failing boundary to the xHCI
Bulk OUT transfer of the 31-byte command block wrapper:

```text
usb-storage: BOT CBW error=0 actual=0 expected=31
usb-storage: sda op=2a lba=38088 blocks=8 error=5 sense=00/00/00
loop1: write block=24 count=8 flags=2 error=5
```

The SCSI command was not rejected by the medium; its CBW was reported as a
zero-byte short completion. The xHCI correction now preserves Chain across a
Link TRB at ring wrap, validates Transfer Events against the active TD's slot,
endpoint, and TRB range, and rejects an OUT short completion as I/O failure.
The focused 254→Link→0 model passes. A separately generated pristine data
image reached the first password prompt and completed a multi-block `/bin/sh`
copy through the overlay without CBW, xHCI, or loop error in headless QEMU.
GUI acceptance with the user's exact command remains required before clearing
this Phase.

## Earlier result, retained as diagnostic history

Three fresh q35/xHCI USB boots reached login, root login completed, and explicit
overlay copies completed without a `loop1`, FAT, UFS, BOT, or xHCI error. A
multi-block `/bin/sh` copy survived a cold QEMU restart and matched its source;
small metadata/data copies also passed on two subsequent fresh images. The same
production image reached login and completed a copy through the PC/i440FX IDE
control path.

The original block-48/block-56 failure did not recur on a writable QEMU backend.
A read-only QEMU USB backend did reproduce the same generic lower-layer `EIO`
family and exposed two concrete defects: USB storage ignored the SCSI
write-protect bit, and private mounts/read-write loops did not propagate that
state. MODE SENSE(6) now marks the disk read-only, mount flags inherit the disk
state, and the read-write `/data.img` loop is rejected early with `EROFS`.
CHECK CONDITION failures now retain and print sense key/ASC/ASCQ instead of
collapsing all evidence into an unexplained `EIO`.

The read-only injection is visibly bounded, and the focused SCSI model fixture
and `make -j16` pass. These remain useful checks, but they do not clear the
reproduced writable-media EIO.
