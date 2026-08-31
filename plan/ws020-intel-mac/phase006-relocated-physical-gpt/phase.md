# WS020 Phase 006: GPT precedence over Protective-MBR extent

Last updated: 2026-08-31

WSID: `ws020`

Phase ID: `p006`

Combined ID: `ws020-p006`

Status: Automatic acceptance complete; one provisional Intel Mac boot pending

Parent: [WS020](../ws.md)

## Objective

Make a valid GPT authoritative over the Protective MBR's advertised extent.
Boot the fixed UEFI-only image after contemporary host software converts its
intentionally primary-only bounded GPT into a complete GPT at the physical end
of a larger USB medium, without changing the published source-image layout or
weakening GPT CRC, geometry, bounds, and copy-consistency validation.

The Protective MBR remains required as the GPT discriminator and must retain a
valid protective entry. Its 32-bit sector count is compatibility metadata, not
the source of truth for the GPT's last LBA.

## Trigger and current diagnosis

The 2026-08-31 Intel Mac observation reached the kernel through UEFI, attached
the 60,549,120-sector USB Mass Storage device as `sda`, and passed the payload
UUID `FDC1-A4EF` from the current p005 image. Partition discovery then reported:

```text
gpt: sda rejected: bounded extent requires both copies primary=3 backup=3
vfs: scan sda H/S=255/63 blocks=60549120: -3 entries
vfs: boot0 selector resolution failed (error 6)
```

In zedBSD, errno 3 is `EINVAL`; neither `3` is a partition count. The pristine
p005 source has a valid primary header at LBA 1 whose alternate LBA is 395,296,
a valid table at LBA 2, and an all-zero declared final reservation. The same
source, enlarged sparsely to exactly 60,549,120 sectors, passes under QEMU.
Consequently, the photographed block device no longer has the pristine GPT
metadata even though its payload UUID is unchanged.

The exact aggregate diagnostic is produced if host software relocates the GPT
to the physical end: LBA 1 names physical LBA 60,549,119 as its alternate, a
backup array/header is written at the physical end, but the Protective MBR
still advertises the compact last LBA 395,296. The old parser derived its only
candidate extent from that MBR, rejected the relocated primary against the
compact alternate-LBA requirement, then read the old zero reservation as the
backup and reported `EINVAL` for both. Windows is known to rewrite GPT metadata
on attached raw-image media, as independently documented in
[Rufus issue 2573](https://github.com/pbatard/rufus/issues/2573), so this is the
leading diagnosis; the photograph alone cannot identify whether Windows, the
image-writing path, or firmware performed the rewrite.

Raw sectors 0, 1, 2--33, 395,264--395,296, and the final 33 sectors would prove
the exact writer and transformation, but are not required for the general
GPT-precedence policy selected by the user.

## Work

1. Keep the production UEFI-only source contract unchanged: pure Protective
   MBR, fixed 395,297-sector extent, valid primary GPT, and zero final
   reservation with no generated backup GPT.
2. After confirming that the disk has one structurally valid protective
   `0xee` entry, read and CRC-check the primary GPT header at LBA 1. When that
   header names an alternate LBA within the physical medium, use that GPT LBA
   as the candidate extent regardless of the Protective MBR's sector count.
3. Validate the primary and its named backup with the ordinary GPT rules:
   header/table CRCs, self/alternate LBAs, usable bounds, table bounds, unique
   identities, non-overlapping partitions, and mutually identical valid
   copies. Preserve the established read-only one-copy recovery policy on both
   exact and bounded physical media: either fully valid named copy is enough.
   The intentional-primary-only recognizer supplies a more specific diagnostic
   for the production source shape; it is not an extra acceptance gate. Do not
   accept two damaged copies or contradictory valid copies.
4. If the CRC-valid primary header fails full header geometry/layout checks, or
   if both the primary copy and its named backup fail full validation, inspect
   at most two backup locations: the PMBR-advertised last LBA when it lies
   inside the medium, and the physical last LBA, with duplicates removed. A
   damaged primary table or entry array does not trigger that search when its
   structurally authenticated named backup is valid; use that backup directly.
   Validate each fallback candidate independently with the ordinary
   backup-header/table/entry rules. Use a uniquely valid candidate read-only;
   if both are valid, require equal GPT identity, geometry, and entry arrays and
   prefer the PMBR-bounded copy. Reject contradictory candidates. Do not scan
   any other LBA.
5. Treat only the Protective MBR extent disagreement as nonfatal. A missing,
   zero-sized, duplicate, wrongly based, or active protective entry remains an
   error. Hybrid compatibility bytes remain allowed by the existing parser.
6. Emit an explicit warning recording `advertised-last`, `gpt-last`, and
   `using GPT`. A matching extent remains quiet. Continue to emit the ordinary
   bounded, damaged-copy, contradiction, and rejection diagnostics.

## Verification

- Extend `HW-T20` with an exact simulated host repair: start from the bounded
  primary-only profile, install/update a valid GPT at the physical end, and
  leave the compact PMBR advertisement unchanged. Require successful
  publication and the exact GPT-precedence warning for 512- and 4096-byte
  sectors and both retained/expanded usable-end forms.
- Cover smaller/larger/saturated PMBR count mismatches, primary-header failure
  with a valid physical-end backup, primary-table failure with a valid named
  backup, CRC-valid but structurally invalid primary-header fallback, valid
  general GPT geometry, ordinary GPT tail partitions, and the
  intentional-primary-only profile with smaller/larger PMBR counts. Exercise
  equal and contradictory PMBR-end/physical-end backup candidates. Prove that
  a valid backup at an unrelated third LBA is never discovered. Continue to
  reject invalid primary discovery with no valid bounded candidate, two
  damaged copies, contradictory headers/arrays, invalid bounds, and
  structurally invalid protective entries. Nonzero or noncanonical declared
  backup reservations must not receive the `intentional primary-only`
  classification, but a fully valid primary remains acceptable through generic
  read-only one-copy recovery; the production byte checker separately enforces
  the source-image shape.
- Retain every existing exact-media, bounded-media, saturated-PMBR, degraded
  copy, contradiction, malformed-entry, and MBR-no-fallback regression.
- Build a disposable larger USB image carrying the simulated complete
  physical-end repair and boot it with OVMF/Q35/xHCI to root overlay, swap,
  init, and `login:`. The expected two partitions and payload UUID must resolve;
  no GPT, VFS, USB-storage, xHCI, panic, or fault failure is allowed.
- Re-run the pristine UEFI larger-media preflight and the six-cell Variant
  matrix after the kernel change. Run the GPT host fixture normally, with
  Address/UndefinedBehavior sanitizers, and with the static analyzer. Run
  `make -j16` and `git diff --check`; do not use aggregate `make check`.
- Refresh p005 and atomically publish only the newly checked pristine source
  image. Record its new byte length, SHA-256, payload UUID, and exact path.
- After all automatic gates pass, request one provisional Intel Mac boot of
  that one frozen image. Do not request repeated physical boots between fixes.

## Result: q047 single automatic run

The no-retry `MAC-T022` run is retained under
[`../temp/p006-q047-final/`](../temp/p006-q047-final/). It used one fresh
private UEFI-only build and one relocated-media OVMF/Q35/xHCI boot; no failed
cell was rerun inside this evidence set.

- The production UEFI-only byte checker passed. The immutable 202,392,064-byte
  source has SHA-256
  `8ec2bd35dbf070c31792a2260764a50a5b9add41f40c8da75c64b641b4959cd5`
  and payload UUID `136D-6390`.
- The runner created a sparse 60,549,120-sector fixture with SHA-256
  `708773a05bda740f6b968f197c14c37578273ee51b19df2ef95d672681e82492`.
  Its Protective MBR remained byte-identical to the source while its valid GPT
  described physical last LBA 60,549,119.
- The guest emitted the exact warning
  `gpt: sda protective MBR extent mismatch: advertised-last=395296 gpt-last=60549119; using GPT`,
  published exactly two GPT partitions, resolved `136D-6390` to `/dev/sda2`,
  mounted `rootfs.img` and `data.img` as the overlay root, activated `swap0`,
  and reached `init: system running`.
- The exact `login:` marker did not appear before the 120-second bound.
  Therefore the runner correctly returned `MAC-T022` **FAIL** with class
  `boot-timeout`; the exact-login oracle was not weakened and this single run
  was not retried.

The GPT/Protective-MBR precedence path, storage publication, root/data/swap,
and init portions of this original automatic check passed. Its terminal
symptom was subsequently reproduced and resolved by
[`ws002-p022`](../../ws002-services/phase022-intermittent-console-login/phase.md):
the USB core's local submit/terminal-publication interrupt handoff could
self-wait after the xHCI CSW completion had arrived. It was not a GPT-parser
failure. The instrumentation-free rerun under
`plan/ws002-services/temp/p022-repair-ordinary-initial/` passes the unchanged
full `MAC-T022` oracle, releasing p006's exact-login prerequisite without
converting the original stopped cell into a pass.

A subsequent parser audit removed the obsolete bounded-media exception from
ordinary one-copy recovery, made full primary-header layout validation precede
extent authority, and added the bounded PMBR/physical fallback when both named
copies fail. The retained host fixture now also proves 512/4096-byte bounded
one-copy recovery, generic non-intentional recovery for noncanonical source
lookalikes, and rejection of a valid backup placed only at an unrelated third
LBA. Ordinary, Address/UndefinedBehavior sanitizer, and static-analyzer runs
all pass.

## Result: post-p022 automatic acceptance

After `ws002-p022` removed the independent USB submit/terminal-publication
self-wait, every remaining automatic gate passed on its first and only run.
No failed cell was retried into a pass.

- `MAC-T022` under `../temp/p006-q047-post-p022-once/` passed in 27 seconds.
  The 202,392,064-byte source SHA-256 is
  `692160cf708904c7444a920022135aad3e10a6f0e78766f88f874a9a6451331d`,
  payload UUID `A93F-BBBE`; its relocated 60,549,120-sector copy is
  `e0c185a6db8ada60566663a93f1771d10a08ea3ca6b7a56e4603ea927931c1a0`.
  The exact mismatch warning, two GPT partitions, `/dev/sda2`, root/data
  overlay, `swap0`, init, and exact `login:` oracle all passed.
- The independent pristine `MAC-T021` run under
  `../temp/p006-q047-post-p022-pristine-once/` passed in 31 seconds with source
  SHA-256
  `3f8e04b981a35037b58dbba3da0339bf79fb37aa6d84988ec5883fb79ba29147`
  and payload UUID `53C2-F273`.
- The uninterrupted six-cell `MAC-T020` run under
  `../temp/p006-q047-post-p022-matrix-once/` passed all three positive and
  three negative firmware/layout cells, with every source post-hash check
  unchanged.
- The exact `MAC-T022` source was atomically published as
  `/home/awe/zedBSD/build/amd64/hdd-image.img` with its recorded SHA-256 and
  UUID. It is the sole image for the next provisional Intel Mac observation.

Automatic implementation and verification are complete. The Phase remains
open only for that one physical observation; it does not block `ws004-p032`.

## Completion conditions

The GPT-precedence fixture and all retained corruption regressions pass, both
the relocated and pristine larger-media QEMU paths reach `login:`, the
six-cell Variant matrix remains strict and passing, and one final provisional
Intel Mac boot of the refreshed frozen artifact reaches ordinary login with
the expected root/data/swap configuration. The p004 five-consecutive-cold-boot
campaign remains a separate final WS acceptance gate.
