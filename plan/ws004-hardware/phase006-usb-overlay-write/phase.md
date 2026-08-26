# WS004 Phase 006: USB loop-backed overlay write reliability

Last updated: 2026-08-26

Phase ID: `ws004-p006`

Status: complete automatic QEMU milestone; manual acceptance pending

Acceptance disposition: **Automatically cleared; manual follow-up pending**

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Eliminate the intermittent, timing-dependent `loop1` write `EIO` when the
production amd64 image boots as q35 xHCI USB mass storage, and prove that the
writable `DATA.IMG` overlay survives ordinary login activity, explicit file
I/O, synchronization, and a cold restart.

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

- [x] Preserve the original and reopened failures, IDE control, BOT/SCSI
      diagnostics, and xHCI Link/event model as diagnostic history.
- [x] Add a diagnostic-build fingerprint. The valid-CSW/caller-zero record and
      generated store order proved the publication boundary without adding
      timing-perturbing per-request output to the production path.
- [x] Prove H1 below: xHCI copied a valid CSW from a nonzero transfer while the
      caller observed success with a stale zero length, and the optimized amd64
      object stored terminal status before `actual_length`.
- [x] Make URB terminal state a release/acquire publication
      boundary: all completion payload is written before the terminal state,
      and every asynchronous observer acquires that state before consuming the
      payload.
- [x] Define a single-owner terminal transition for completion versus
      timeout/cancel. A timeout must not overwrite a concurrent successful
      completion or free an URB/DMA request until HCD quiescence is confirmed.
- [x] Add focused models for reordered completion publication and the
      completion-versus-cancel race. Include a deterministic legacy failure and
      a corrected high-iteration case, rather than relying on host timing alone.
- [ ] Establish the repeated-boot harness against the unfixed image, including
      at least one classified failure, before using it to claim a fix.
- [x] Run the post-fix 500-boot HW-T12 gate sequentially from byte-identical
      pristine raw-image copies using the exact q35/xHCI/SMP=4/NE2000 topology.
- [x] Run the explicit overlay write/readback/cold-persistence case, IDE
      control, focused fixtures, `make -j16`, and `git diff --check`; do not use
      `make check`.
- [x] Remove or compile out perturbing trace noise, retain concise failure
      diagnostics, and update `ws004-p005`, HW-T11/HW-T12, WS004, and M1 only
      after every gate passes.

## Completion conditions

- Five hundred sequential fresh USB boots reach `login:` and complete the
  declared post-login settling interval with zero `loop1`, UFS, FAT, BOT, SCSI,
  xHCI, or syslog write-error markers. Every run starts from a byte-identical
  pristine raw-image copy; repeatedly booting one mutated image does not count.
- The repeated-boot result contains 500 classified runs, not 500 attempted
  launches. A missing build fingerprint, truncated debug log, early QEMU exit,
  or login timeout is a harness failure and never a pass.
- A bounded multi-block create/write/fsync/readback workload passes and its
  contents survive a QEMU cold restart of the same disposable image.
- The test crosses the `DATA.IMG` loop/FAT backing path; a tmpfs-only write does
  not count.
- IDE attachment of the same image remains correct.
- Failure injection or read-only media still returns a visible bounded error;
  the fix must not report false success.
- Focused tests, relevant configured builds, `make -j16`, and
  `git diff --check` pass. The repository-wide `make check` target is not used.

The 500-run gate is an operational regression threshold, not proof that the
failure probability is mathematically zero. Any matching failure resets the
post-fix run count after the next code change.

## Latest acceptance finding

User acceptance on 2026-08-25 first suggested a fresh-image first-boot defect,
but subsequent runs on 2026-08-26 established that it is timing-dependent: a
newly generated image can pass or fail, and the failure is not tied to one
fixed boot point. All failing cases use the production-style q35/xHCI boot with
four virtual CPUs and an ISA NE2000 device.

Observed failures now cover three Bulk-Only Transport phases:

```text
usb-storage: BOT CBW error=0 actual=0 expected=31
usb-storage: BOT data dir=out error=0 actual=0 expected=4096
usb-storage: BOT CSW error=0 actual=0 status=0 tag=800 expected-tag=800
usb-storage: sda op=2a lba=38120 blocks=8 error=5 sense=00/00/00
loop1: write block=56 count=8 flags=2 error=5
```

This invalidates the earlier QEMU clearance. The three passing runs were an
insufficient sample and the read-only-media propagation fix did not address
this writable-media defect. Diagnosis resumes with this exact topology; the
Phase remains Uncleared until repeated acceptance passes without any storage
error. The earlier xHCI correction preserves Chain across a Link TRB at ring
wrap, validates Transfer Events against the active TD's slot, endpoint, and TRB
range, and rejects an OUT short completion as I/O failure. Its focused
254→Link→0 model remains useful, but the new failures prove that it did not
clear this defect.

## Fault localization and hypotheses

The valid-CSW case is the strongest boundary observation. `csw` is zeroed before
the Bulk IN operation, and the xHCI driver copies its bounce buffer into that
object only when its computed `actual` is nonzero. Seeing the expected nonzero
tag and status therefore shows that xHCI copied the CSW, while the synchronous
USB caller subsequently observed `actual_length == 0` and success. This places
the leading fault between HCD completion and synchronous URB consumption, not
at the disk medium, SCSI command, or overlay layer.

The current optimized amd64 object provides a concrete mechanism. Although the
C source assigns `actual_length` before terminal `status`, generated code stores
`status` first and `actual_length` second. Both fields are plain memory, and the
wait loop uses only `hal_compiler_barrier()`, which is not inter-CPU
synchronization. On SMP=4, the waiter can therefore observe success and consume
the old zero length before the interrupt CPU publishes the length.

| Priority | Hypothesis | Evidence and prediction | Disposition rule |
| --- | --- | --- | --- |
| H1 — highest | URB completion payload is published after terminal status | Generated amd64 store order and valid CSW with caller-visible zero length match exactly; SMP=1 should greatly suppress the window | Correlate HCD/core/wait values. Repair with an explicit release/acquire terminal-publication contract only if the trace agrees |
| H2 | Completion, timeout, and cancel have no single terminal owner | Current plain state checks and unconditional timeout assignment can overwrite a concurrent completion or permit premature free; this does not best explain `error=0`, but is adjacent correctness debt | Exercise deterministic completion/cancel interleavings and require confirmed HCD quiescence before free |
| H3 | xHCI event ownership, residual decoding, or ring wrap still reports the wrong transfer | Remains possible if the HCD-correlated record itself has `actual=0`; the previous Link/event patch passing one model is insufficient | Inspect event pointer, slot, endpoint, completion code, residual, TD range/cycle, and HCD-computed length for the same request ID |
| H4 | BOT/SCSI/media or loop/FAT is the origin | IDE is a control and BOT rejects a success/zero-length result before upper layers can complete the write; valid CSW contents contradict a medium failure | Revisit only if the correlated USB completion is internally consistent and nonzero while BOT observes otherwise |
| H5 | Test artifact does not contain the intended diagnostic build | Screenshots contain BOT diagnostics, but exact binary identity has not been machine-checked | Require a unique diagnostic schema/build fingerprint in every accepted run |

## Debugging and verification strategy

1. Freeze one pristine base image after `make -j16`; record its digest, QEMU
   version, diagnostic fingerprint, and the complete user-supplied command.
   Create a new raw copy for each run and verify that the base image digest does
   not change.
2. Use a bounded per-request record rather than unconditional per-transfer
   printing. Correlate a monotonic ID and CPU ID across BOT phase, xHCI enqueue,
   Transfer Event, HCD-computed result, USB-core terminal publication, and
   waiter consumption. On a BOT mismatch, dump the relevant record once.
3. Run SMP=1 and SMP=4 characterization with identical media and topology. This
   is diagnostic evidence only; SMP=1 passing does not clear the SMP=4 gate.
4. Inspect the corrected object code as well as the C source. The terminal state
   must be the final release publication, and all polling/status access must use
   the matching atomic contract; mixing atomic and plain accesses is forbidden.
5. Make timeout/cancel testing deterministic with controlled producer steps.
   A waiter may return only after either normal completion owns the terminal
   state or the HCD confirms dequeue/quiescence; no path may overwrite a terminal
   success or free live DMA/URB state.
6. Implement the repeated-boot harness under `plan/ws004-hardware/tests/`, not
   `.internal/`. Run sequentially to avoid changing the host-scheduling
   workload, bound each boot, and wait through a declared interval after
   `login:` so late init/syslog writes are observed.
7. Capture port `0xe9` debug-console text as the primary oracle. On amd64,
   `hal_cons_putc()` emits the same character to debugcon before updating VGA,
   so text parsing is more exact than OCR. A failure screenshot/OCR may be kept
   as corroboration, but OCR alone cannot classify a run because scrolling,
   font recognition, and capture timing can hide an error.
8. Classify every run as pass, USB/storage failure, boot timeout, early QEMU
   exit, or harness error. Preserve the full log and disposable image for each
   failure, plus an aggregate machine-readable record containing run number,
   image digest, elapsed time, first failure marker, and complete command.
9. The primary failure expressions include `loop1: write ... error=`, BOT
   length/status errors, SCSI WRITE(10) errors, xHCI non-success/residual errors,
   and `syslogd: ... Input/output error`. A visible prompt never overrides one
   of these markers.

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

## q009 implementation and acceptance result

The H1 mechanism is confirmed. In the pre-fix optimized amd64 object,
`drv_usb_hcd_complete()` published terminal `status` before writing
`actual_length`; the waiting CPU used plain loads plus a compiler barrier. The
valid-CSW failure is decisive corroboration: the xHCI IN bounce buffer could
only have populated the expected CSW tag when its computed transfer length was
nonzero, yet the synchronous caller observed success and zero length.

The USB core now gives each submission a single atomic terminal owner. The
winner writes completion payload and `actual_length`, then release-publishes
the terminal status; waiters and accessors acquire that status before consuming
the payload. Timeout/cancel can no longer overwrite a concurrent completion.
The xHCI dequeue path obtains and claims the active request under its active
lock rather than dereferencing a potentially stale URB-private request first.
The corrected object stores `actual_length` before terminal status.

The focused model deterministically exposes the legacy stale-zero outcome,
passes 200,000 corrected release/acquire publications, and passes 2,000
completion-versus-timeout ownership races. The xHCI and USB-storage SCSI
focused models and `make -j16` also pass.

The 1,000-run HW-T12 attempt was stopped after run 36 rather than misreported as
a USB result. Runs 1–25 and 27–36 reached login with no USB/storage marker. Run
26 hit an amd64 #GP in `remove_free()` immediately after syslogd startup; the
allocator's `next_free` link was invalid. Rebooting the retained run-26 image
with the same topology reached login, excluding persistent overlay-media
damage. The stress classifier now reports such faults as `kernel-failure`
instead of `boot-timeout`.

Accordingly, at q009 closure the original USB symptom had zero recurrences in
35 completed post-fix boots, but the Phase remained **Uncleared**: 35 boots were
not the then-required 1,000, and one independent kernel fault invalidated the
gate. q010 subsequently executed
[`ws004-p008`](../phase008-smp-heap-integrity/phase.md) and restarted HW-T12 at
run 1; its final result follows.

## q010 final automatic result

`ws004-p008` proved and corrected the SMP heap fault which interrupted q009.
The user revised the automatic threshold from 1,000 to 500 boots, reserving
more detailed behavior for manual acceptance. The rebuilt, byte-identical-base
q35/xHCI/SMP=4/NE2000 gate recorded 501 consecutive passes before stopping:
the accepted first 500 plus one additional pass. There were zero kernel,
USB/storage, or harness failures, and the pristine base digest was unchanged.

Focused URB, xHCI, SCSI, and heap regressions pass; SMP=1 USB, SMP=4 USB, and
SMP=4 PC/IDE controls each pass 10/10. This completes the automatic QEMU Phase
conditions. The user's later interactive acceptance is explicitly not claimed.
See [q010 HW-T12 evidence](../tests/q010-hwt12-evidence.md).
