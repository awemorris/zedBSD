# WS003 Phase 024: PC-9821V13 Stage-1 fixed-read compatibility

Last updated: 2026-08-31

Phase ID: `ws003-p024`

Status: Planned; Queue-ready after the active Queue closes

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), extending `BR-T54` and `BR-T55`

## Objective

Restore entry on the NEC PC-9821V13 by removing the unnecessary AH=`84h`
SENSE transaction from the LBA-0 Stage-1 IPL. Stage 1 reads one fixed physical
sector, CHS 0/0/2, and does not consume geometry; it should therefore issue
that bounded AH=`06h` read directly. Preserve every later geometry-dependent
SENSE path, the native PC-98 disk layout, and the already-correct `IPL1`
signature.

The implementation decision is closed and can enter a Queue without another
human design choice. The Phase still ends with one exact physical boot because
QEMU already accepts both the old and current Stage-1 sequences.

## Established boundary

- `IPL1` is present at LBA-0 offsets 4--7 in both the built Stage 1 and the
  installed image. Its positive and corrupted-negative checks pass.
- The ordinary and diagnostic images reach `login:` under the maintained
  qemu-pc98 configuration, while the PC-9821V13 still beeps and stops before
  loader text.
- Stage 1 currently owns a private stack, preserves the firmware entry
  registers and boot-device identity, performs AH=`84h` SENSE, and then reads
  the invariant CHS 0/0/2 sector. None of the returned geometry is used.
- The earlier zedBSD Stage-1 path and the independently inspected native
  PC-98 fixed-disk IPL pattern enter their fixed CHS read without this
  preliminary SENSE. The new SENSE call is therefore the narrowest remaining
  firmware-compatibility difference at the observed boundary.
- `partition-pbr.S` and `bootzbsd.S` use SENSE for geometry-dependent later
  work. Their validation is not implicated by this Stage-1 result and must
  remain intact.

This Phase deliberately supersedes only p022's Stage-1 SENSE requirement. It
does not rewrite p022/p023's historical evidence or relax any later disk
validation.

## Fixed decisions

1. Delete Stage 1's `bios_sense` call, helper, `1S` failure path, and dead
   diagnostic branch.
2. Keep the private `SS:SP`, saved `SI`/`DI`, normalized fixed-disk `boot_id`,
   explicit `ES:BP`, `BX=512`, AH=`06h`, CHS 0/0/2, carry check, and `1R`
   read-failure diagnostic.
3. Do not change `lba2.S`, `partition-pbr.S`, or `bootzbsd.S` merely to make
   them resemble Stage 1. In particular, strict SENSE remains in the PBR and
   BOOTZBSD paths which actually consume disk geometry or enumerate devices.
4. Retain the native PC-98 LBA-0/LBA-1/LBA-2 layout, `IPL1`, boot-menu bytes
   `09 00`, trailing `55 aa`, native partition entries, and `BOOT` selection.
   Do not add a PC/AT partition entry or GPT compatibility path.
5. Keep successful production boot silent. The diagnostic variant retains
   finite Stage-1-entry, Stage-1-read-failure, and Stage-2-entry checkpoints;
   it no longer advertises a SENSE-failure checkpoint that cannot occur.
6. Do not add a guessed reset, delay, geometry substitution, or retry loop.
   Any further physical stop is localized by the existing bounded diagnostic
   path before another repair is designed.

## Implementation plan

1. Simplify `bootloader/pc98/disk-ipl.S` to prepare the target and issue the
   fixed AH=`06h` read immediately after boot-device normalization.
2. Remove only the resulting dead Stage-1 SENSE code and update diagnostic
   comments/groups without changing the Stage-1 sector budget.
3. Update the focused source/binary contract so it rejects AH=`84h` in Stage
   1, requires the complete fixed AH=`06h` CHS 0/0/2 transaction, and proves
   that SENSE still exists in `partition-pbr.S` and `bootzbsd.S`.
4. Re-run exact `IPL1`, `09 00`, `55 aa`, native LBA-1/LBA-2, no-PC/AT-entry,
   installed-byte, and fixed-size checks. A separately compiled Stage 1 must
   still be exactly 512 bytes.
5. Build the ordinary PC-98 image with `make -j16`, run its production image
   checker, and boot a disposable copy under the maintained qemu-pc98 command
   through `init: system running` and `login:` with no IPL failure marker.
6. Rebuild the diagnostic variant, prove Stage-1 and Stage-2 entry markers in
   qemu-pc98, then copy it to a uniquely named immutable handoff path and
   record its size and SHA-256.
7. Ask for one PC-9821V13 boot of that exact artifact. A loader/kernel screen
   clears this compatibility Phase; a bounded failure group identifies the
   next Phase. Do not request repeated hardware boots here.

The focused Stage-1 object/layout checks may run independently of the current
WS008 host-CLI regression. If that unrelated regression prevents the ordinary
integrated `make -j16` or maintained Noct runners, record the exact blocked
gate and leave this Phase `uncleared`; do not patch Noct inside WS003.

## Completion conditions

- LBA-0 Stage 1 contains no AH=`84h` SENSE transaction or `1S` branch;
- its one AH=`06h` read still publishes the complete fixed CHS 0/0/2 and
  buffer/register contract, checks carry, and reports `1R` on return failure;
- PBR and BOOTZBSD SENSE/geometry validation remain byte- and source-checked;
- Stage 1 remains 512 bytes and the image retains `IPL1`, `09 00`, `55 aa`,
  the native partition table/selector, and zero PC/AT partition entries;
- focused positive/negative fixtures, the production image checker, and
  `git diff --check` pass;
- the ordinary disposable image reaches init/login under the maintained
  qemu-pc98 command and the diagnostic image reaches both entry markers;
- one uniquely named/hash-recorded diagnostic artifact advances beyond the
  original beep boundary on the PC-9821V13.

## Reconsideration boundary

Stop and extract a new Phase if the physical result reaches Stage 2 and then
fails in its partition-table/PBR path, if the fixed AH=`06h` call returns an
explicit error, or if the medium is not exposed as the expected fixed-disk
BIOS device. None of those outcomes authorizes weakening the PBR/BOOTZBSD
SENSE checks or changing the native partition format in this Phase.
