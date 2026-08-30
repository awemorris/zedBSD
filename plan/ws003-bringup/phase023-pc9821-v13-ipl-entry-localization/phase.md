# WS003 Phase 023: PC-9821V13 IPL entry localization

Last updated: 2026-08-31

Phase ID: `ws003-p023`

Status: Uncleared (`q039`); every automatic gate passes, awaiting one exact
PC-9821V13 diagnostic-image observation

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), especially `BR-T55`

## Objective

Remove ambiguity from the still-failing PC-9821V13 boot without inventing a
different disk signature. Prove that the normal Stage 1 and installed LBA 0
contain ASCII `IPL1` at offsets 4--7, then produce one separately named,
immutable diagnostic image whose audible checkpoints distinguish firmware
handoff, Stage-1 SENSE/read failure, and Stage-2 entry in a single physical
boot.

## Established facts

- The PC-9800 hardware reference places `IPL1` at address `0004h` in a
  fixed-disk IPL record.
- FreeBSD/pc98 places `.ascii "IPL1"` at offset 4.
- The maintained qemu-pc98 IDE and SCSI ROMs compare only offsets 4--7 before
  entering the IPL.
- The current zedBSD Stage 1 and disk image contain bytes
  `49 50 4c 31` at those exact offsets. The signature itself is therefore
  correct and must not be changed as a guessed repair.
- q037's fixture checked the tail and installed bytes but did not independently
  assert or negatively corrupt the four-byte `IPL1` field.
- The mutable `build/pc98/hdd-image.img` no longer has q037's recorded hash, so
  a physical result cannot be attributed safely unless this Phase publishes a
  new uniquely named artifact and hash.

## Fixed decisions

1. Retain the native PC-98 layout, `IPL1` at offsets 4--7, and `55 aa` at
   offsets 510--511.
2. Describe offsets 508 and 509 accurately as boot-menu version and reserved
   bytes. They are not a count of Stage-1 sectors.
3. Add an exact positive check for `IPL1` to both Stage 1 and installed LBA 0,
   plus a one-byte-corruption negative fixture which the checker rejects.
4. Keep tracing outside the normal production path. A build-time diagnostic
   variant may share maintained assembly, but ordinary `disk-image` output
   must remain silent and byte-stable apart from intentional unrelated source
   changes.
5. The diagnostic variant emits a bounded, countable audio group only after
   it owns a safe stack. It distinguishes at least Stage-1 entry, SENSE
   failure, LBA-2 read failure, and Stage-2 entry. It also writes the same
   checkpoint to QEMU debug port E9 so automatic execution can validate the
   selected path without interpreting host audio.
6. The physical handoff names exactly one diagnostic image and SHA-256. One
   V13 boot is requested; repeated human boots are deferred until a new fact
   justifies them.

## Implementation plan

1. Extend `BR-T54`/the production BIOS checker with the exact `IPL1` contract
   and a corrupted-copy rejection case; correct stale metadata terminology.
2. Add conditional Stage-1/Stage-2 audio/debug checkpoints which compile out
   of the normal image. Preserve the no-inherited-stack, register, size, and
   INT 1Bh contracts from p022.
3. Add a bounded PC-98 diagnostic-image target which starts from the checked
   normal image and replaces only the fixed IPL sectors with their diagnostic
   variants. Never publish it as the ordinary image.
4. Run `make -j16`, the production checker, `BR-T54`, and qemu-pc98 through
   login for the normal image. Run the diagnostic image far enough to prove
   its entry checkpoints and continuation automatically.
5. Copy the finished diagnostic image to a stable, explicit handoff pathname,
   record its size and SHA-256, and stop at one physical observation.

## Completion conditions

- exact positive and corrupted-negative `IPL1` checks pass for Stage 1 and the
  installed image;
- normal Stage 1 remains 512 bytes, retains its native layout and signatures,
  and the production PC-98 image reaches login under qemu-pc98 without an
  audio checkpoint;
- the diagnostic image has countable Stage-1/Stage-2 checkpoints, continues
  under qemu-pc98, and has a stable name, size, and SHA-256;
- `make -j16`, `make check-disk-image`, focused fixtures, and
  `git diff --check` pass;
- one boot of that exact diagnostic artifact on the PC-9821V13 reports the
  audible group or advances into the loader/kernel.

The Phase remains `uncleared` at the last condition until the user supplies
that one observation. An audio result defines the next disk/firmware boundary
without authorizing a partition-layout rewrite.

## Automatic result (2026-08-31)

The signature hypothesis is closed: both the ordinary Stage 1 and installed
LBA 0 contain `IPL1` at offsets 4--7. The production checker rejects a copy in
which the same one byte is corrupted in both places, so byte equality alone
cannot hide a bad magic field. The normal image retains `09 00 55 aa`, passes
its production checker, and reaches `login:` in the maintained qemu-pc98.

`ZEDBSD_PC98_IPL_DIAGNOSTIC` compiles entirely out of the normal fixed IPLs.
The Phase-owned Make fragment builds separate Stage-1/Stage-2 binaries and
patches only LBA 0 and LBA 2--15 of a copied normal image. Its finite audio
groups use the PC-98 BIOS timed-beep service after each stage owns its stack;
the same checkpoints appear as `P1E` and `P2E` on debug port E9. A disposable
qemu-pc98 run emitted exactly those two entry markers, no failure marker, and
continued through `init: system running` and `login:`. The immutable source
hash was unchanged by that run.

Verification evidence:

| Gate | Result |
| --- | --- |
| `make -j16` | PASS |
| production `make check-disk-image` | PASS |
| `BR-T54` exact/corrupted `IPL1` fixture | PASS |
| `BR-T55` diagnostic byte-range contract | PASS |
| maintained BR-T46 `pc98/default` | PASS 1/1 to `login:` |
| `BR-T55` diagnostic qemu-pc98 runner | PASS: `P1E`, `P2E`, init, login |
| `git diff --check` | PASS |

## Physical handoff: one boot only

Purpose: determine which earliest fixed-disk IPL boundary the PC-9821V13
actually reaches. Boot only this file once:

`/home/awe/zedBSD/build/handoff/ws003-p023-pc9821-v13-ad49c654.img`

| Property | Value |
| --- | --- |
| Size | 135,266,304 bytes |
| SHA-256 | `ad49c6542234d521f654be110e47703f27cdf73d3766209da413edd459888f9c` |

Report the audible groups in order, or a photo if the loader/kernel advances:

- no beep: firmware did not transfer control to zedBSD Stage 1;
- `1`, then `2`: Stage 1 entered, then SENSE failed;
- `1`, then `3`: Stage 1 entered, then the LBA-2 read failed;
- `1`, then `4`: Stage 2 entered; if the machine then advances, the fixed IPL
  path is clear;
- after `1`, `4`, a group of `5`, `6`, or `7`: respectively partition-table
  read, BOOT-name lookup, or partition-PBR read failed.

Do not run a second physical trial for this Phase. The first result is the
resume fact for a later bounded repair.
