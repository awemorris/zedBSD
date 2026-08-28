# WS018 Phase 002: disk-label and platform ownership

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p002`

Combined ID: `ws018-p002`

Status: Planned; not selected in a Queue

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
