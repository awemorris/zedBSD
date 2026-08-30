# WS020 Phase 002: amd64 BIOS, Hybrid, and Apple UEFI image layouts

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p002`

Combined ID: `ws020-p002`

Status: Completed (2026-08-30)

Parent: [WS020](../ws.md)

## Objective

Make only disk-image composition variant-dependent and introduce the compact,
primary-only GPT artifact required for the Apple UEFI path.

## Work

1. Give the image backend explicit layout and declared-capacity arguments.
   Arithmetic uses checked 64-bit sectors and rejects partitions, GPT metadata,
   or selected capacity that cannot be represented safely.
2. Preserve the existing Hybrid image contents and boot behavior.  Generate
   BIOS-only with no GPT/ESP/UEFI payload, and UEFI-only with no executable MBR
   bootstrap, BIOS partition, zedBSD custom PBR loader, or `BOOTZBSD.EXE`.
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

## Implemented contract

- `hybrid` remains the previously accepted 203,423,744-byte self-contained
  primary+backup GPT artifact.  `bios` remains the 135,266,304-byte legacy MBR
  artifact.  For these two formats the selected GiB value is validated as a
  deployment-media constraint; it deliberately does not change their bytes or
  on-disk partition geometry.
- `uefi` is always a compact 202,375,168-byte artifact.  Its primary GPT
  declares the selected 2--256-GiB medium exactly, ends usable space 33 sectors
  before that declared end, and contains only the front ESP and FAT32 payload.
  No backup GPT bytes, executable MBR bootstrap, active/hybrid MBR entry, BIOS
  loader partition, zedBSD custom BIOS PBR, or `BOOTZBSD.EXE` are published.
  The ordinary FAT32 BPB/VBR emitted by the formatter remains, but exposes no
  reachable zedBSD BIOS boot path.
- Both BIOS stage-1 forms and the UEFI loader are always build artifacts.
  Image composition selects `stage1-native.bin` only for BIOS-only and the
  existing GPT-aware `stage1.bin` only for Hybrid; source selection is invariant.
- PC/AT Stage 1 carries a fixed 16-byte `ZBL1` metadata record at MBR offset
  `0x1a0`.  The loader itself reads `stage2_lba` from that record into its DAP,
  while the backend and checker validate magic, version, size, machine, and
  layout-specific LBA.  The metadata is therefore the single runtime source of
  truth rather than a description duplicated beside movable instruction bytes.
- The production image rule uses a content-stable layout/capacity stamp and
  validates the complete candidate before atomically replacing
  `hdd-image.img`.  Failed candidates and their temporary files are removed
  without replacing an accepted destination.
- Explicit layout backend calls freeze the public 129-MiB/128-MiB legacy
  geometry and reject legacy native-root or fragmentation switches.  Specialized
  legacy fixtures retain those switches only through calls without `--layout`.
- The GPT reader recognizes intentional primary-only media only for the exact
  128-entry x 128-byte conventional geometry and an all-zero final 33-sector
  region at the declared physical end.  Existing generic read-only recovery
  for a damaged GPT copy remains intact, but a nonzero/unreadable damaged copy
  is never reported as intentionally absent and valid contradictory copies
  remain rejected.

## Deployment consequence

The compact UEFI file does not physically reach the selected medium's final 33
sectors.  A raw copy onto previously used media therefore cannot clear stale
backup-GPT bytes by itself.  The p004 writer/instructions must explicitly zero
the final 33 sectors before or after copying the compact artifact.  A freshly
zeroed sparse materialization has the required semantics; a nonzero final
region does not qualify as intentional primary-only GPT.

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

## Result

- `MAC-T010` generated and checked Hybrid and BIOS at both capacity endpoints,
  and UEFI-only at all eight supported capacities.  Exact artifact extents,
  MBR/GPT bytes and CRCs, partition geometry, FAT type/content, and the required
  loader inclusion/exclusion matrix passed.
- `MAC-T011` rejected corrupt primary header/table, CRC-correct wrong alternate
  LBA, CRC-correct overlapping and out-of-file partitions, cross-layout images,
  a forbidden BIOS loader in UEFI payload, damaged Hybrid ESP backup VBR,
  wrong Stage-1 LBA in both directions, corrupt `ZBL1` metadata, missing fixed
  payload inputs, materialized-size candidates, and a nonzero final backup
  region.  Atomic-publication failure preserved the prior destination and left
  neither candidate temporary path behind.
- The production GPT host fixture passed ordinary, Address/UndefinedBehavior
  sanitizer, and static-analyzer runs, including exact 2- and 256-GiB sparse
  primary-only cases, physical-size minus/plus-one rejection, and a
  noncanonical near-miss that stays on generic degraded-copy recovery.
- `MAC-T001` was rerun after adding `stage1-native.bin` to the always-built
  loader family; hashes, object paths, and compile contracts remained identical
  across all three Variants and the 2/256-GiB capacity endpoints.
- Both formal runners snapshot `config.mk` and place all mutable target/image
  artifacts under their caller-supplied evidence namespace.  Concurrent
  `MAC-T001` and `MAC-T010`/`MAC-T011` runs with distinct evidence paths passed,
  proving that no shared `hdd-image.img`, contract stamp, or target BUILD race
  remains.

## Completion conditions

Each layout contains exactly its declared boot mechanisms, the UEFI artifact
is compact and capacity-correct after materialization, and no kernel or loader
source selection depends on Variant.
