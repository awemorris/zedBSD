# WS019 Phase 005: QEMU NVMe overlay-install acceptance

Last updated: 2026-08-29

Phase ID: `ws019-p005`

Status: planned; dependency-gated

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Prove the complete non-formatting installer-v1 transaction and installed boot
on a disposable QEMU NVMe before any physical Latitude write.

## Fixture

- ordinary amd64 USB image as installer source;
- one NVMe image prepared in advance with valid GPT, exactly one FAT32 ESP,
  one separate FAT32 payload, and unmanaged sentinel content on both;
- fresh OVMF variable store captured before installation;
- no zedBSD managed file present before the transaction.

## Required cells

1. install succeeds and preserves GPT, labels, unmanaged files, and NVRAM;
2. installed fallback loader discovers the same-disk `/zedbsd.cfg`, consumes
   `kernel=vmunix`, binds omitted `boot0` and bare image paths to that FAT,
   mounts
   `rootfs.img` plus `data.img`, activates `swapfile`, and reaches login;
3. an absent config fails visibly; duplicate configs warn and choose the first
   deterministic same-disk candidate, while the installer preflight refuses to
   create such an ambiguous installed layout;
4. conflicting file, insufficient space, wrong filesystem, source/target
   alias, and injected copy/flush/verify failures never report success;
5. an auxiliary FAT disk with matching-looking files cannot steal selection
   from the ESP's physical GPT disk.

QEMU may supply a firmware-created boot choice for the disk; the installer
itself must not change the variable store. A successful manual/fallback file
selection is sufficient and must be distinguished from a zedBSD-created
`Boot####` entry.

## Completion conditions

- One reusable WS019 test runner records every cell and immutable artifact
  digest.
- The production `make -j16` build and the full installed overlay boot pass.
- The exact candidate image for WS003 p018 is recorded.

## Reconsideration boundary

Return to design if OVMF cannot exercise the no-installer-Boot-variable path,
if payload selection is nondeterministic, or if any unmanaged byte changes.
