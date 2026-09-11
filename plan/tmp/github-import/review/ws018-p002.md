# WS018 Phase 002: disk-label and platform ownership

Last updated: 2026-08-29

WSID: `ws018`

Phase ID: `p002`

Combined ID: `ws018-p002`

Status: Complete (`q026`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Remove partition-table parsing and reusable device implementation from
platform-owned kernel directories.  Give disk-label formats a common driver
owner and represent each supported platform with exactly one initialization
translation unit under `src/kern/platform/`.

## Entry evidence

- MBR and Sun parsers currently live in `src/kern/`, while PC-98 and X68k
  parsers live below their historical platform directories.
- PC/AT and RPi4 select the MBR scheme, sun4u selects the Sun scheme, X68k
  selects its native scheme, and PC-98 selects an auto scheme that may choose
  PC-98 or MBR parsing.
- `src/kern/{pcat,pc98,rpi4,sun4u,x68k}/platform.c` already implements the
  platform hook set, but the PC/AT and PC-98 directories also contain fonts or
  disk-label code that belongs to drivers.

## Fixed design

- The canonical format directory is `src/drivers/disklabel/`; do not create a
  parallel `parttable/` directory.
- MBR, Sun, PC-98, PC-98 auto-detection, and X68k on-disk parsers live there.
  The generic scheme registry, partition objects, and scan policy remain in
  `src/kern/partition.c` and `include/kern/partition.h`.
- Scheme declarations shared with platform initialization use one focused
  driver header rather than platform-private headers.  Their existing scan
  signatures and externally observed partition fields remain unchanged.
- `src/kern/platform/` contains `pcat.c`, `pc98.c`, `rpi4.c`, `sun4u.c`, and
  `x68k.c`.  PC/AT continues to serve both i386 PC/AT and amd64; no redundant
  architecture-named platform file is introduced.
- Each platform C file defines `kern_platform_init()` and its complete
  `kern_platform_*` hook set.  No common C implementation or helper header is
  stored in `src/kern/platform/`.
- `src/kern/platform/README.md` records the one-C-file-per-platform rule, the
  prohibition on common implementation there, and the ownership boundary for
  reusable code.  The README is the deliberate non-C exception.
- PC-98 auto-detection order and fallback, boot-origin partition numbering,
  PARTUUID/PARTLABEL publication, and all platform initialization order remain
  behaviorally identical.
- Graphics-owned PC/AT and PC-98 font code must already have moved in p009;
  this Phase does not restore it to a platform directory.

## Implementation procedure

1. Capture partition fixtures and platform build manifests before relocation,
   including PC-98 media that exercises both native and MBR fallback paths.
2. Move each on-disk parser to `src/drivers/disklabel/`, establish one focused
   declaration header, and repair scheme references without changing the
   generic partition registry.
3. Move each existing platform hook implementation into its single canonical
   `src/kern/platform/<platform>.c` file.  Preserve initialization and device
   registration order exactly.
4. Add `src/kern/platform/README.md` with the fixed ownership rules.
5. Repair every platform manifest and maintained focused test.  Remove the
   historical platform directories only after no source, private header, or
   build prerequisite remains in them.
6. Run disk-label fixtures, the platform source audit, supported build gates,
   and representative boot checks.

## Verification

- `KA-T010`: maintained images for MBR, PC-98, Sun, and X68k disk labels
  produce the same partition count, start/data block, size, boot flag, UUID,
  and label before and after relocation.  PC-98 native/MBR auto selection is
  covered separately from the X68k native-label fixture.
- `KA-T011`: `src/kern/platform/` contains only its README and one C file per
  platform; historical platform implementation directories are absent; font
  ownership is under graphics drivers; no common platform implementation is
  present.
- Compile/link every supported platform and confirm its selected partition
  scheme and device initialization sequence from bounded boot diagnostics.
- Run `make -j16` and `git diff --check`; do not run `make check` or consume
  `.internal/` material.

## Completion conditions

- All on-disk partition/disk-label parsers have driver ownership under the one
  canonical directory, while generic registry/policy remains kernel core.
- Every supported platform is represented by exactly one platform C file with
  the complete hook set and documented ownership rules.
- The historical `src/kern/{pcat,pc98,rpi4,sun4u,x68k}/` implementation
  directories are gone.
- Disk-label results and supported platform builds/boots are behaviorally
  unchanged, and `KA-T010`/`KA-T011` pass.

## Dependencies

- `ws018-p001` establishes `src/drivers/`.
- `ws018-p009` moves platform-owned graphics/font implementation before the
  historical PC/AT and PC-98 directories are deleted.

The numeric Phase order does not authorize running p002 before these
dependencies.  A future Queue must select only dependency-ready work.

## Reconsideration boundary

Stop for human review if a disk-label format requires platform policy inside
its parser, if PC-98 auto detection cannot be preserved, if one platform
cannot express its complete hook set in one C file, or if a stable partition
identity changes.  Do not introduce a common platform source or a second
disk-label directory as a workaround.

## q026 execution record (2026-08-29)

Completed without reaching the reconsideration boundary.

- MBR/GPT, PC-98 native, PC-98 auto-selection, Sun, and X68k parsers now live
  under the sole `src/drivers/disklabel/` implementation directory.  Their
  shared declarations and X68k decoder constants are consolidated in
  `include/drivers/disklabel.h`; the generic registry and scan policy remain
  in `src/kern/partition.c` and `include/kern/partition.h`.
- `src/kern/platform/` now contains only its ownership README and the five
  platform translation units `pcat.c`, `pc98.c`, `rpi4.c`, `sun4u.c`, and
  `x68k.c`.  PC/AT continues to serve both i386 and amd64.  Each built object
  exports all seven declarations from `<kern/platform.h>` and selects exactly
  its prior disk-label scheme.
- The historical `src/kern/{pcat,pc98,rpi4,sun4u,x68k}/` directories and the
  five private disk-label headers were removed after all six manifests were
  repaired.  The parser and platform bodies compare equal to their entry
  versions except for replacement by the consolidated include, so scan policy
  and device-registration order did not change.  The p009-owned PC/AT and
  PC-98 fonts remain below `src/drivers/graphics/`.

`KA-T010` passed before and after relocation.  Its maintained host fixture
links the production parsers against deterministic memory disks and covers
MBR PARTUUID/extents, protective-MBR GPT GUID/UTF-16 labels, PC-98 native CHS
extents and flags, authoritative `IPL1` selection, MBR fallback and native
fallback, Sun checksum/geometry/slices, and X68k 4096-byte labels with
1024-byte-to-512-byte extent conversion.  Counts, indexes, start/data blocks,
sizes, flags, UUIDs, and labels remained equal.

`KA-T011` passed: the two canonical directories have their exact allowed
contents, every platform source has the complete hook set and expected scheme
mapping, no historical directory/header or parallel `parttable/` owner
remains, all active manifests use the canonical paths, and font ownership is
under the graphics drivers.

Fresh, separate compile/link builds passed:

| Target | Selected scheme | Result |
| --- | --- | --- |
| amd64 PC/AT | MBR | kernel link and amd64 checker pass |
| i386 PC/AT | MBR | kernel link and PC/AT contract checker pass |
| i386 PC-98 | PC-98 auto | stage-2 link and patch checker pass |
| RPi4 arm64 | MBR | kernel link and arm64 checker pass |
| sun4u sparcv9 | Sun | kernel link and SPARC V9 checker pass |
| X68k m68k | X68k native | kernel link and m68k checker pass |

The normal configured `make -j16` rebuilt the amd64 disk image.  A disposable
copy booted under `qemu-system-x86_64`, preserved platform device discovery,
reported the expected four-entry MBR scan and `sda1` extent, started init, and
reached `login:`.  `git diff --check` passed.  Neither `make check` nor
`.internal/` was used, and no Noct source was inspected or modified.
