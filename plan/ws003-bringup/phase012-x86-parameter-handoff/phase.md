# WS003 Phase 012: x86 boot-parameter handoff

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p012`

Combined ID: `ws003-p012`

Status: Completed (`q015`, 2026-08-27)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Carry the same bounded textual parameter string into kernel-owned storage on
i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI without borrowed-pointer or
loader-lifetime assumptions.

## Scope

- versioned handoff extensions for all four required x86 paths;
- a maximum of 3071 ASCII bytes plus NUL;
- preservation and copying of an i386 Multiboot command line;
- a versioned inline command-line extension for the custom PC-98 handoff;
- versioned ZBL6 BIOS and UEFI handoffs with a bounded inline string or a
  separately bounded loader-owned block copied before reuse;
- strict conversion of UEFI `LoadOptions` when it contains a UTF-16 command
  string;
- a loader-origin `boot0` identity fallback; and
- the generated legacy-layout default root/swap selection only when the
  platform's native parameter source is absent or empty.

## Non-goals

- parsing the future Boot CPAR section menu or VFAT long filenames;
- making legacy PC/AT or PC-98 implement the CPAR menu;
- interpreting the parameter values inside architecture code;
- accepting arbitrary binary UEFI OptionalData as text; or
- changing root/swap behavior before p013/p014.

## Design

The HAL publishes `boot.command-line` from kernel-owned storage on every
target. No HAL returns a pointer into reclaimable Multiboot, loader, UEFI pool,
or low bootstrap memory after initialization.

Each handoff remains versioned and validates its size, string length, NUL, and
flags before copying. Old handoff versions remain accepted and synthesize the
compatibility default together with the architecture's loader-origin `boot0`.
An invalid new handoff fails visibly instead of being reinterpreted as an old
version.

When an image has no explicit root selection, its loader supplies the current
layout as text:

```text
overlay-root=boot0:rootfs.img overlay-data=boot0:data.img \
swap0=boot0:swapfile
```

This is a compatibility default emitted by the loader/HAL boundary, not a VFS
filename heuristic. An absent or empty native parameter source selects exactly
this default. A nonempty Multiboot, custom-loader, or UEFI `LoadOptions` string
is the complete replacement and is never merged with the default. It must
therefore carry one complete root mode; the common parser rejects an incomplete
replacement. This gives every target one precedence rule without resolving
duplicate known keys in architecture code.

## Work packages

1. Add production-shared handoff validation fixtures for exact maximum length,
   missing NUL, invalid size/flag combinations, old versions, and copy
   lifetime.
2. Copy i386 PC/AT Multiboot text instead of retaining its raw pointer.
3. Extend the PC-98 loader handoff without changing its partition/device
   meaning.
4. Extend the amd64 BIOS ZBL6 path while preserving compact legacy versions
   and framebuffer/boot-UUID extensions.
5. Add the amd64 UEFI handoff version, strict `LoadOptions` text conversion,
   and the same kernel-owned HAL publication.
6. Emit and test the current-layout default string on every generated x86
   image path.
7. Preserve automatic loader-origin `boot0` when `boot0=` is absent.

## Verification

- Add BR-T43 handoff fixtures sharing the production validation/conversion
  helpers for all four paths.
- Prove old and new handoff versions independently.
- Poison or overwrite the source buffer after HAL initialization and prove the
  published string remains intact.
- Run `make -j16` and build all four target images.
- Run minimal amd64 BIOS and UEFI QEMU marker boots showing the same normalized
  parameter string. The production PC/AT and PC-98 runtime cells, together
  with full root/swap acceptance, belong to p015.
- Run `git diff --check`; do not run `make check` or use `.internal/`.

## Completion conditions

- all four x86 HALs publish a bounded kernel-owned `boot.command-line`;
- no new handoff trusts an unbounded pointer or unterminated input;
- old supported handoff versions remain valid;
- every generated current-layout image receives the explicit overlay/swap
  default; and
- BR-T43, all four target build/link paths, the amd64 BIOS/UEFI marker boots,
  and BR-T46 production runtime acceptance on all four paths pass.

## q015 execution evidence

- The shared fixed-size parameter record now carries one bounded, inline,
  NUL-terminated command line. The i386 PC/AT Multiboot path copies its source,
  PC-98 uses its compatible extended handoff, and the amd64 BIOS and UEFI paths
  preserve older supported handoff versions while accepting the current one.
- UEFI `LoadOptions` conversion rejects malformed UTF-16 and publishes only
  validated text. All HAL publications use kernel-owned storage rather than a
  loader-lifetime pointer.
- BR-T43 passed its maximum-length, missing-NUL, malformed-record, old-version,
  conversion, and copy-lifetime coverage.
- The amd64 kernel, BIOS stage2, `BOOTZBSD.EXE`, and UEFI `BOOTX64.EFI`
  build/link checks passed. The i386 PC/AT kernel, `BOOTZBSD.EXE`, and
  payload64 checks passed; the i386 PC-98 kernel, `BOOTZBSD.EXE`, and payload32
  checks passed.
- Each of the four production artifacts contains the exact explicit string
  `rootpart=/dev/sda1 init=/bin/sh` when requested and the exact generated
  overlay/swap default otherwise.
- Production amd64 BIOS and amd64 UEFI marker boots passed and exposed the
  normalized parameter string. Authoritative BR-T46 subsequently passed all
  31 cells, including production PC/AT and PC-98 runtime and all four paths'
  default and explicit parameter strings.
- `git diff --check` passed.

## Dependencies and handoff

Depends on `ws003-p011`. p013 and p014 consume the transported values. UEFI
Boot CPAR later replaces the fixed UEFI default with a menu-derived string but
uses this same handoff.

## Reconsideration boundary

Stop if one legacy loader cannot reserve a bounded parameter block without a
layout/size redesign, or if UEFI firmware supplies non-text OptionalData that
cannot be distinguished safely from an intended command string.
