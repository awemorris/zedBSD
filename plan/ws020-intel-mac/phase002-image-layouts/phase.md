# WS020 Phase 002: amd64 BIOS, combined, and Apple UEFI image layouts

Last updated: 2026-08-31

WSID: `ws020`

Phase ID: `p002`

Combined ID: `ws020-p002`

Status: Completed (revised 2026-08-31)

Parent: [WS020](../ws.md)

## Objective

Make only disk-image composition Variant-dependent and provide the fixed,
pure-Protective-MBR, primary-only GPT artifact required for the Apple UEFI
path.

## Work

1. Give the image backend an explicit layout argument only. Remove declared
   capacity parsing, arithmetic, command options, stamps, and checker inputs.
2. Preserve the accepted combined UEFI+BIOS image contents and behavior.
   Generate BIOS-only with no GPT/ESP/UEFI payload, and UEFI-only with no
   executable MBR bootstrap, compatibility partition, BIOS partition, zedBSD
   custom PBR loader, or `BOOTZBSD.EXE`.
3. In UEFI-only, emit a zero bootstrap/signature area, one non-active `0xee`
   record from LBA 1 through the declared fixed end, three zero records, and
   `55 aa`.
4. Preserve the current ESP and payload extents. Set total geometry to 395,297
   sectors: primary header at LBA 1, table at LBA 2, first usable at LBA 34,
   payload ending at last usable LBA 395,263, and alternate LBA 395,296.
5. Reserve LBAs 395,264--395,296 as 33 zero sectors and deliberately omit the
   backup table/header. Publish atomically only after a Variant-aware checker
   validates the exact layout and inclusion/exclusion rules.
6. Keep the kernel's strict intentional-primary-only classification. Accept
   this form on both exact-sized and larger physical media; reject a declared
   end beyond the medium and malformed primary geometry/CRCs. On a larger
   bounded medium, a nonzero invalid declared-tail is not an intentional
   omission and is rejected. Preserve the established exact-medium
   valid-primary/damaged-backup read-only recovery independently.

## Verification

- Byte-level fixtures validate exact PMBR bytes, GPT header/table CRC and
  geometry, partition types/extents, loader/file inclusion and exclusion,
  fixed file length, and all-zero final reservation.
- Host GPT fixtures cover exact-sized and larger-physical-media intentional
  primary-only input, and reject undersized media, malformed primary data,
  noncanonical PMBRs, and nonzero malformed declared-tail metadata.
- Cross-Variant images and failed candidates do not replace an accepted output.
- Existing combined BIOS+UEFI behavior remains passing; all loader artifacts
  remain present regardless of selected image Variant.
- `make -j16`, focused fixtures, and `git diff --check` pass.

## Completion conditions

Each layout contains exactly its declared boot mechanisms, UEFI-only has the
fixed pure Protective MBR and intentional primary-only GPT contract, larger
physical media are safe, and no kernel or loader source selection depends on
Variant.

## Prior result and revision

The first q036 implementation encoded a selected 2--256-GiB end in a shorter
sparse artifact. That capacity axis was withdrawn on 2026-08-31. Its strict
layout separation, stable Stage-1 metadata, atomic publication, and GPT
validation remain useful; only capacity-dependent geometry and exact-medium
requirements are replaced by the fixed contract above.

## Revised result

- `MAC-T010/MAC-T011`: PASS for all three fixed layouts. The independent
  checker proved exact extent, MBR/GPT bytes and CRCs, partition bounds,
  loader/file inclusion and exclusion, FAT32 primary/backup VBRs, Stage-1
  metadata, and atomic publication. It rejected corrupt and semantically
  inconsistent GPTs, cross-layout loaders, short/long UEFI files, an arbitrary
  nonzero final reservation, and even a valid backup GPT in the published
  source artifact.
- Fixed artifact extents are 203,423,744 bytes (`hybrid`), 135,266,304 bytes
  (`bios`), and 202,392,064 bytes (`uefi`). The UEFI-only source has exactly
  395,297 sectors and the final 33 sectors are zero.
- The production GPT host fixture passes ordinary, Address/UndefinedBehavior
  sanitizer, and static-analyzer runs. Exact and larger physical media accept
  only the strict pure-PMBR primary-only form; bounded nonzero reservations,
  compatibility entries, MBR code, noncanonical table geometry, and truncated
  media fail. Exact-medium degraded-copy recovery remains intact.
- The image backend compiles with strict warnings, focused fixtures and
  `make -j16` pass, and `git diff --check` is clean.

Evidence is retained under `../temp/p002-revised-final/`; the GPT fixture uses
only WS020-local temporary storage because the host-global `/tmp` is full.
