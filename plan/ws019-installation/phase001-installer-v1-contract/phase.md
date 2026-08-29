# WS019 Phase 001: installer v1 product and safety contract

Last updated: 2026-08-29

Phase ID: `ws019-p001`

Status: planned; discussion/design only; not ready for a Queue

Parent: [WS019 installation and disk administration](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Freeze the exact initial disk layouts and mutation rules before exposing a raw
partition writer or installer. This Phase produces a reviewed specification
and ordered implementation P books; it does not implement commands or write a
disk.

## Already fixed

- `/bin/zedinst` runs from the normal USB-booted system.
- `/sbin/diskpart` supports GPT only.
- Guided installation offers existing-partition use or explicit whole-disk
  erase; it offers no dual-boot, resize, move, or automatic target selection.
- USB boot is the recommended non-destructive trial path.
- Whole-disk UEFI v1 uses protective MBR plus primary/backup GPT and a FAT32
  EFI System Partition; hybrid and legacy-MBR layouts are excluded.
- The installed fallback loader path is `EFI/BOOT/BOOTX64.EFI`.
- Device partitions remain one-based; the first NVMe partition is
  `/dev/nvme0n1p1`.
- Existing-partition mode is GPT-only and names every selected destination
  explicitly by its current partition node and stable PARTUUID.

## Decisions to finish

1. Freeze the fixed whole-disk partition map, sizes/alignment, GPT type GUIDs,
   attributes, FAT32 ESP contents, native UFS partition, and swap/data
   placement. Allocate stable zedBSD-specific type GUIDs where no standard
   type applies.
2. Decide whether the initial native root is formatted to the destination size
   by a target UFS1 formatter, or seeded from the existing UFS1 root image with
   an explicitly documented fixed-size limitation. A formatter is preferred
   for a useful internal installation.
3. Define existing-partition mode precisely: which filesystems and minimum
   sizes are accepted, whether selected filesystems are reformatted, what
   existing files may be replaced, and how every destructive extent is shown
   before confirmation. No coexistence promise is implicit.
4. Freeze the block-admin UAPI: 64-bit geometry, parent/partition flags and
   offsets, stable identity, exclusive whole-disk mutation claim, mounted/swap
   descendant refusal, flush, safe partition rescan, and exactly-once raw
   writes.
5. Freeze confirmation and automation rules. Interactive whole-disk mode must
   require an exact device-specific erase phrase; any future noninteractive
   mode needs both explicit confirmation and stable identity, not `--force`
   alone.
6. Define failure/recovery states across protective MBR, primary/backup GPT,
   rescan, format, copy, config, and verification. Partial installation is
   visible and never reported as success.
7. Define the source-artifact contract by which a USB-booted `zedinst` locates
   its own `BOOTX64.EFI`, kernel, root image/tree, data image, and swap payload.
8. Freeze the QEMU NVMe matrix for both installed native and overlay entries.
9. Decide whether v1 relies on firmware fallback/manual selection only or may
   create one zedBSD `Boot####` entry. In either case it must not reorder or
   delete unrelated firmware entries implicitly.

## Suggested fixed layout for discussion

The smallest layout that can exercise both required boot modes on one disk is:

1. GPT entry 1, standard EFI System Partition, FAT32, containing
   `EFI/BOOT/BOOTX64.EFI`, the fixed kernel, `boot.cfg`, `rootfs.img`,
   `data.img`, and optional file-backed swap;
2. GPT entry 2, zedBSD UFS1 native root, populated from the same staged base
   system;
3. optional raw swap only if p001 selects it instead of the already implemented
   `boot0:swapfile` model;
4. remaining GPT entries unused; no hybrid MBR aliases.

The menu can then offer a native entry using `rootpart=/dev/nvme0n1p2` and an
overlay entry using `boot0` files. This is a proposal, not yet a frozen layout.
The user's original `rootpart=/dev/nvme0n1p0` example is corrected by the
existing one-based convention; if only one partition is created it is `p1`,
while this two-partition proposal places native UFS at `p2`.

## Completion conditions

- Every decision above has one unambiguous answer, including exact destructive
  extents and recovery behavior.
- WS019 p002--p007 each receive a bounded P book with dependencies, tests,
  completion conditions, and reconsideration boundaries.
- WS004, WS013, WS003, WS009, and the master dependency/status entries agree
  with the frozen contract.
- No code implementation or destructive test is claimed by this design Phase.
