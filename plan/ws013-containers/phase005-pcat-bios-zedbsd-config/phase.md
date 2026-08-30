# WS013 Phase 005: PC/AT BIOS `zedbsd.cfg` boot unification

Last updated: 2026-08-30

Phase ID: `ws013-p005`

Status: planned; not in the current Queue; depends on `ws013-p003` and
`ws013-p004`

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Complete two independently accepted PC/AT goals:

1. i386 PC/AT reaches its existing payload PBR/`BOOTZBSD.EXE`, and that
   `BOOTZBSD.EXE` reads root `/zedbsd.cfg` instead of a fixed kernel name and
   embedded parameter record;
2. amd64 BIOS reaches the payload PBR/`BOOTZBSD.EXE`, and that
   `BOOTZBSD.EXE` reads the identical `/zedbsd.cfg` format instead of using
   the current reserved-area direct kernel loader.

The modern hybrid GPT/MBR disk then boots the same payload FAT through either
firmware path:

```text
UEFI firmware -> ESP/EFI/BOOT/BOOTX64.EFI -> payload FAT/zedbsd.cfg
BIOS firmware -> MBR -> payload FAT PBR -> BOOTZBSD.EXE -> zedbsd.cfg
```

The selected configuration, kernel, root image, data image, swap file, and
kernel-parameter result must not depend on whether the machine entered through
UEFI or legacy BIOS. “Same format” is an externally observable parser and
handoff contract; it does not require the UEFI, i386, and amd64 loaders to
share implementation source.

## Current-state correction

There is no implemented PC/AT `boot.cfg` reader. `BOOTZBSD.EXE` currently
looks up a fixed root-directory `VMUNIX` name and emits a build-time embedded
boot-parameter record. The i386 PC/AT image already uses a chain loader which
selects the active MBR entry, enters its PBR, and lets that PBR load
`BOOTZBSD.EXE`. The amd64 hybrid image currently uses a different direct
Stage-2 kernel-loading path from the BIOS boot area. This Phase removes that
behavioral split; it does not preserve a fictitious `boot.cfg` compatibility
contract.

## Target on-disk layout

The modern amd64 hybrid image has two distinct FAT roles:

1. an EFI System Partition containing `EFI/BOOT/BOOTX64.EFI`;
2. the following zedBSD payload FAT32 containing `ZEDBSD.CFG`, `VMUNIX`,
   `ROOTFS.IMG`, `DATA.IMG`, `SWAPFILE`, and `BOOTZBSD.EXE`.

The hybrid MBR has one active FAT entry referring to the payload FAT, and its
BIOS path reaches that partition's zedBSD PBR and then `BOOTZBSD.EXE`. The GPT
and protective-MBR invariants remain valid. The ESP deliberately has no
`zedbsd.cfg`, so p002 discovers the same-disk payload FAT. Whether a very small
internal chain sector remains between the MBR bootstrap and PBR is an
implementation detail only if the externally validated path still selects
the active payload partition; the old direct-to-kernel amd64 Stage 2 is not a
supported fallback.

The ordinary i386 PC/AT image keeps one active payload FAT, but that FAT gains
`ZEDBSD.CFG` and uses the same PBR/`BOOTZBSD.EXE` configuration path.

An extra q031 preflight reached the current i386 PC/AT kernel but did not
publish a physical boot disk, so the default `boot0` lookup failed. The
generated MBR medium was byte-identical before and after the q031 fixture-tool
change, while amd64 SeaBIOS still reached `login:`. Before changing the i386
loader, this Phase must reproduce and restore that baseline or split the
driver regression into an explicit prerequisite; loader acceptance cannot be
claimed from kernel entry alone.

## Configuration contract

`BOOTZBSD.EXE` opens root `/zedbsd.cfg` on the FAT from which it was loaded
(FAT lookup remains case-insensitive).
The language, bounds, diagnostics, and normalization are the authoritative
v1 contract frozen by `ws013-p003`:

- exactly one loader-only `kernel=` line;
- all other lines become the existing textual kernel-parameter record;
- omitted `boot0=` synthesizes the current payload FAT's canonical UUID;
- bare `overlay-root`, `overlay-data`, and boot-file `swapN` values receive
  `boot0:` while explicit boot slots and raw swap selectors are preserved;
- `rootpart=` and `init=` pass through unchanged;
- malformed, missing, over-limit, or unreadable configuration and missing or
  invalid configured kernel are visible fatal errors;
- no embedded parameter or fixed `VMUNIX` fallback remains.

FAT lookup is case-insensitive. A configuration accepted by p003, including a
safe relative kernel subdirectory, must not acquire a second, narrower BIOS
grammar. If satisfying the common path contract requires bounded directory or
LFN support in `BOOTZBSD.EXE`, that support belongs to this Phase rather than
silently restricting `kernel=`.

## Loader and handoff constraints

- Preserve the PBR guarantee that `BOOTZBSD.EXE` is an ordinary loadable MZ
  file and keep its maximum size within the PBR's documented bounded load.
- Retain the current runtime ELF32/ELF64 selection behavior: an i386 image
  supplies its i386 `vmunix`, while an amd64 image supplies its amd64
  `vmunix`.
- Populate the existing boot-parameter record dynamically and retain the FAT
  volume serial/UUID handoff. Do not add a second parameter ABI.
- Keep BIOS disk I/O bounded and visibly fail on short reads, malformed FAT,
  fragmented data unsupported by a remaining loader primitive, or overflow.
- Do not write the disk, NVRAM, configuration, or filesystem during boot.

## Verification

- Host fixtures feed the same valid and invalid configuration corpus used by
  p003 to the BIOS implementation and compare the exact emitted parameter
  record.
- i386 PC/AT BIOS QEMU boots overlay and native-`rootpart` configurations
  through MBR/PBR/`BOOTZBSD.EXE` and reaches the expected init.
- The same amd64 hybrid image boots once through SeaBIOS and once through
  OVMF; both logs show the same selected FAT UUID, configured kernel, and
  exact parameter text.
- The amd64 image checker proves that the ESP and payload FAT are distinct,
  the hybrid active entry addresses the payload FAT, its PBR is present, and
  required files occur on the intended partition.
- Missing/invalid `ZEDBSD.CFG`, missing configured kernel, and an ESP-only
  stray configuration fail according to the frozen selection rules, with no
  fixed-name or embedded fallback.
- Existing IDE and xHCI USB-root BIOS regressions still reach `login:`.

## Completion conditions

- i386 PC/AT boots through active partition PBR -> `BOOTZBSD.EXE` ->
  `/zedbsd.cfg` and meets the p003 grammar exactly.
- amd64 BIOS boots through active payload PBR -> `BOOTZBSD.EXE` ->
  `/zedbsd.cfg`, meets the p003 grammar exactly, and no longer uses the
  reserved-area direct-kernel path as a production fallback.
- One produced amd64 hybrid image boots successfully through both UEFI and
  BIOS paths from the common payload FAT.
- The production image tools and checkers encode the two-FAT layout rather
  than relying on manual image mutation.
- Focused fixtures, `make -j16`, and the declared BIOS/OVMF QEMU cells pass.

## Reconsideration boundary

Return to planning rather than weakening the shared configuration language if
the PBR load ceiling cannot hold a safe parser/path implementation, if the
hybrid MBR cannot address the payload FAT without violating GPT validity, or
if supporting the p003 kernel path requires a separately agreed FAT/LFN
loader contract.
