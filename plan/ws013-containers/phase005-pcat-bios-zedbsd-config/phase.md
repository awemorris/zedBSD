# WS013 Phase 005: PC/AT BIOS `zedbsd.cfg` boot unification

Last updated: 2026-08-30

Phase ID: `ws013-p005`

Status: Completed (`q032`, 2026-08-30)

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

The reproducible production-loader gate is:

```sh
plan/ws013-containers/tests/run-pcat-zedbsd-config-qemu.sh \
    build/ws013-p005-acceptance
```

The migrated boot-parameter regression cells can be selected independently:

```sh
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh \
    --platform pcat --case default OUTPUT
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh \
    --platform pcat --case native OUTPUT
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh \
    --platform amd64-bios --case default OUTPUT
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh \
    --platform amd64-uefi --case default OUTPUT
```

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

## Actual implementation and evidence

- The i386 PC/AT and amd64 BIOS production paths now share the active-payload
  chain MBR -> FAT PBR -> `BOOTZBSD.EXE` -> root `/zedbsd.cfg`. The loader
  consumes the p003 parser result directly, synthesizes the selected FAT UUID,
  and resolves the configured kernel through bounded FAT12/16/32 directory,
  LFN, subdirectory, and cluster-chain traversal. The old embedded parameter
  record and fixed `VMUNIX` policy are not fallbacks.
- The amd64 hybrid image now has a distinct ESP and following zedBSD payload
  FAT32. Its protective GPT/MBR remains valid, the hybrid active MBR entry
  addresses the payload FAT, and both SeaBIOS and OVMF select the same payload
  `/zedbsd.cfg`, kernel, root/data images, swap file, and FAT identity. The
  public image checker validates both GPT copies, CRCs, usable ranges, GUIDs,
  hybrid entries, PBR executable regions, stored FAT filename case, and file
  placement.
- The payload PBR validates BPB geometry, reserved-area capacity, FAT/root
  bounds, contiguous `BOOTZBSD.EXE` extent, its fixed MZ/ZBL2 envelope, declared
  image size, and full checksum before transfer. `BOOTZBSD.EXE` bounds every
  file/cluster read and rejects malformed ELF header/program-header extents,
  physical destinations outside 1 MiB--1 GiB, inconsistent amd64 high-window
  mappings, and entries outside executable `PT_LOAD` ranges. ELF32 BSS is
  cleared through unreal-mode GS; ELF64 BSS is cleared after entering the
  one-GiB identity map.
- All BIOS helper objects and final loaders link as ELF32 i386 with an explicit
  i386 ISA ceiling; no object-format conversion remains. The PC/AT chain
  sector uses dedicated `stage2-chain.*` artifacts, so a dirty incremental
  tree containing the retired direct-kernel `stage2.o` cannot reuse it. Image
  checkers compare the complete installed Stage 2 with that declared input.
  They also compare PC/AT Stage 1 executable bytes and its boot signature,
  excluding only the builder-owned disk-signature/partition-table range.
  The complete linked
  image ends at `stage2_end=0x5b91`, below the protected C-stack floor at
  `0xd000`. The superseded unreferenced direct-kernel
  `bootloader/pcat/stage2.S` source was deleted, while the small chain sector
  remains as the MBR-to-PBR mechanism.
- `run-pcat-zedbsd-config-qemu.sh` passed 20/20 production cells at
  `build/p005-pcat-acceptance7`: i386 overlay
  and native root, the same amd64 hybrid image through SeaBIOS IDE, SeaBIOS
  xHCI USB, and OVMF xHCI USB, safe LFN/subdirectory lookup, configuration and
  kernel failures, PBR checksum/BPB failures, and ELF file/destination bounds.
  `run-zedbsd-config-host-test.sh` passed ordinary, ASan/UBSan, and static
  analysis; `run-fat-directory-host-test.sh` passed ordinary and sanitizer
  runs. The migrated BR-T46 image-helper dry runs passed for PC/AT, amd64 BIOS,
  and amd64 UEFI.
- Production images are built under an atomic validation boundary: the backend
  writes an unchecked sibling, the platform checker validates it, and only a
  successful check publishes the target. An intentional checker failure
  preserved the prior target SHA-256 and left neither unchecked sibling nor
  backend temporary file. The GPT FAT, generic BIOS FAT, and native-UFS
  comparison paths remove stale extraction files before every check and clean
  them after both success and failure; deliberate mismatches left no extraction
  file and an immediate correct-input retry passed. Backend and checker
  start/nonzero failures likewise preserved the prior image and removed both
  `.unchecked` siblings. Public aliases likewise copy to a sibling and rename.
  GPT disk and partition GUIDs are generated as nonzero, mutually distinct
  UUIDv4 values per medium; the checker enforces their form, primary/backup
  equality, and separation, and two same-input images had four different IDs.
  Both the GPT and generic BIOS checkers rejected one-byte Stage 1 mismatches;
  the GPT checker also rejected the native-root Stage 1 as the expected input
  for the hybrid image.
- A normal full amd64 `config.mk` rebuild regenerated the root staging tree,
  rootfs tarball, amd64 UFS image, kernel, BIOS loader, and final hybrid image.
  `make -j16`, the production GPT checker, the public `check-disk-image` target,
  shell syntax checks, and `git diff --check` all passed.
- Direct DOS launch of the PC/AT MZ executable is not part of the production
  PBR acceptance. Its legacy stub currently maps a DOS drive letter to a BIOS
  disk by arithmetic, which is not correct for arbitrary DOS configurations.
  A later compatibility Phase must either resolve the DOS current volume
  explicitly or retire that direct entry; q032 does not claim it as supported.

## Reconsideration boundary

Return to planning rather than weakening the shared configuration language if
the PBR load ceiling cannot hold a safe parser/path implementation, if the
hybrid MBR cannot address the payload FAT without violating GPT validity, or
if supporting the p003 kernel path requires a separately agreed FAT/LFN
loader contract.
