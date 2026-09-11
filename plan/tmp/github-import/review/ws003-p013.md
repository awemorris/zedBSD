# WS003 Phase 013: boot slots and root-source selection

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p013`

Combined ID: `ws003-p013`

Status: Completed (`q015`, 2026-08-27)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Replace implicit single-boot-partition and fixed overlay-image selection with
`boot0`--`boot3`, `rootpart`, `overlay-root`, and `overlay-data` while retaining
safe loop, overlay, and block-identity invariants.

## Scope

- four sparse boot-filesystem slots;
- automatic loader-origin `boot0` and explicit slot overrides;
- `/dev`, UUID, label, PARTUUID, and PARTLABEL resolution;
- FAT16/FAT32 validation and duplicate-slot rejection;
- bounded `bootN:PATH` normalization;
- mutually exclusive native `rootpart` and overlay-root modes;
- configurable read-only lower and read-write upper image attachment;
- removal of fixed `/rootfs.img` and `/data.img` selection from the kernel;
- visible diagnostics naming the parameter and slot that failed; and
- focused VFS/selector fixtures plus QEMU smoke tests.

## Non-goals

- swap activation;
- FAT32/VFAT LFN bootloader menu implementation;
- runtime exposure of boot slots at `/boot`;
- more than one lower or upper overlay layer;
- ephemeral upper filesystems; or
- changing the existing block-identity ambiguity rule.

## Design

Resolve all explicitly defined boot slots after physical disks and partitions
exist but before attaching image files. `boot0` falls back to the loader-origin
partition when omitted. Validate resolved slots as distinct FAT filesystems and
retain private mounts for as long as any attached image or later swap source
uses them.

The root-mode validation is atomic. Exactly one mode is selected. Native root
mounts the `rootpart` block device directly and refuses any overlay parameter.
Overlay mode requires two valid boot-file references, attaches the lower
read-only and upper read-write, then creates the existing one-lower/one-upper
overlay. A failure releases every partial mount, path, disk, and loop object.

## Work packages

1. Add pure helpers for indexed names, block selectors, `bootN:PATH`
   normalization, sparse slots, and root-mode validation.
2. Add a four-entry private boot-slot table with duplicate physical-partition
   detection and balanced mount lifetime.
3. Move current automatic boot-origin selection behind the `boot0` fallback.
4. Rename the current `root=` native selector to `rootpart=` and remove the old
   public name.
5. Replace fixed root/data image constants with the two explicit overlay file
   references.
6. Preserve read-only lower, writable upper, recursion, and nested-overlay
   rejection.
7. Add exact failure cleanup and bounded boot diagnostics.

## Verification

- Add BR-T44 production-shared fixtures for selector grammar, four sparse
  slots, duplicate aliases, undefined cross-slot references, path traversal,
  root-mode exclusion, and complete rollback after each injected stage error.
- Boot disposable QEMU images in native-root and overlay modes before p015's
  full platform matrix.
- Reorder disks and prove UUID/PARTUUID selectors do not regress to discovery
  order.
- Run existing loop/overlay/storage fixtures, `make -j16`, and
  `git diff --check`.
- Do not run `make check` or use `.internal/`.

## Completion conditions

- all four boot slots obey the reference contract;
- undefined, duplicated, ambiguous, non-FAT, and unsafe paths fail visibly;
- `rootpart` never creates an overlay;
- overlay mode requires and uses the requested lower and upper files;
- no fixed root/data filename remains in kernel selection logic; and
- BR-T44, regressions, build, and representative QEMU boots pass.

## q015 execution evidence

- The production `boot-source` contract and ownership layer implements four
  sparse private FAT16/FAT32 slots, loader-origin `boot0`, explicit override,
  duplicate-partition rejection, normalized `bootN:PATH` lookup, slot
  retention, reverse teardown, and same-FAT native-root promotion.
- Root selection now consumes the common p011 parameter object. The former
  `boot=`/`root=` parsing, UFS marker search, fixed image-name selection, and
  implicit FAT-root fallback are absent from the VFS path.
- Native `rootpart` and explicit overlay lower/upper validation share one
  pre-commit ownership boundary. Overlay failures unwind paths, files, loops,
  image mounts, and boot-slot mounts while preserving the original error.
- BR-T44 passed by directly linking the production parameter and boot-source
  implementation. BR-T42 also passed as a parser regression.
- `make -j16` passed, including the amd64 kernel and `BOOTX64.EFI` checks and
  image generation. `git diff --check` passed.
- The generated amd64 image reached `login:` in both legacy BIOS and 4-GiB
  OVMF Q35/xHCI USB-storage boots. Both logs show loader-origin `boot0`, the
  requested `boot0:rootfs.img`/`boot0:data.img` attachments, and the explicit
  overlay-root diagnostic.

The ordinary image builder creates overlay-root media, so the focused native
mode evidence remains the production-shared root-mode and same-FAT-promotion
fixture. Authoritative BR-T46 then passed end-to-end default overlay, native
root, and invalid-root cells on all four production paths. Its UUID and
PARTUUID auxiliary-disk-first cells passed on both amd64 firmware paths,
confirming loader-origin `boot0` and explicit `boot1` independently of disk
discovery order.

## Dependencies and handoff

Depends on `ws003-p011` and `ws003-p012`. p014 reuses the boot-slot table for
file-backed swap. p015 completed the four-platform acceptance.

## Reconsideration boundary

Stop if supporting four slots requires a public mount namespace/UAPI, if a
selected boot filesystem cannot be kept alive without leaking a global mount,
or if the existing overlay recursion guard cannot represent the selected
files safely.
