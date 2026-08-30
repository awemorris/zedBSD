# WS020 Phase 002: amd64 BIOS, Hybrid, and Apple UEFI image layouts

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p002`

Combined ID: `ws020-p002`

Status: Planned after `ws020-p001`

Parent: [WS020](../ws.md)

## Objective

Make only disk-image composition variant-dependent and introduce the compact,
primary-only GPT artifact required for the Apple UEFI path.

## Work

1. Give the image backend explicit layout and declared-capacity arguments.
   Arithmetic uses checked 64-bit sectors and rejects partitions, GPT metadata,
   or selected capacity that cannot be represented safely.
2. Preserve the existing Hybrid image contents and boot behavior.  Generate
   BIOS-only with no GPT/ESP/UEFI payload, and UEFI-only with no BIOS code,
   BIOS partition, PBR, or `BOOTZBSD.EXE` payload.
3. In UEFI-only, zero the protective-MBR bootstrap area, emit one covering
   `0xee` entry with no active flag, retain `55 aa`, and emit only primary GPT
   header/table plus ESP and payload entries.
4. Set the primary GPT `alternate_lba` to selected-capacity sectors minus one
   and reserve the conventional final 33 sectors from usable space, while
   deliberately omitting backup entries/header from the compact file.
5. Keep the compact file long enough to contain every populated front extent,
   but not the selected empty tail.  Publish atomically only after a variant-
   aware checker validates contents and exact absence/presence rules.
6. Teach the kernel GPT reader to distinguish the strictly valid intentional
   primary-only shape from a present corrupt backup, without weakening bounds,
   CRC, duplicate-table, or conflicting-copy rejection.

## Verification

- Byte-level fixtures validate MBR entries/code/signature, GPT CRC/geometry,
  partition types/extents, loader/file inclusion and exclusion, compact file
  length, selected declared last LBA, and zero unwritten backup area after
  materialization.
- Corrupt primary header/table, wrong alternate LBA, undersized materialized
  medium, nonzero malformed backup, overlapping/out-of-file partition, and
  unexpected cross-variant loader cases fail without publishing an image.
- Existing Hybrid BIOS+UEFI checks remain passing.
- The production disklabel fixture accepts only the intentional primary-only
  contract and preserves strict rejection of contradictions.
- `make -j16` and `git diff --check` pass.

## Completion conditions

Each layout contains exactly its declared boot mechanisms, the UEFI artifact
is compact and capacity-correct after materialization, and no kernel or loader
source selection depends on Variant.
