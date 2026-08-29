# WS019 Phase 001: overlay installer-v1 contract

Last updated: 2026-08-29

Phase ID: `ws019-p001`

Status: completed by design, 2026-08-29

Parent: [WS019 installation and disk administration](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Freeze the first installer as one non-formatting FAT-overlay path and split
its implementation into bounded Phases. This Phase changes plans only; it
does not implement commands or write a disk.

## Fixed result

- `/bin/zedinst` runs from the normal USB system.
- The destination is one existing GPT disk with exactly one usable existing
  FAT32 ESP and one explicitly selected distinct existing FAT32 partition.
- The ESP receives only `/EFI/BOOT/BOOTX64.EFI` as a managed path.
- The payload FAT32 receives only `/vmunix`, `/boot.cfg`, `/rootfs.img`,
  `/data.img`, and `/swapfile` as managed paths.
- `ZEDBSD` names the payload role; v1 does not require or change a GPT name or
  FAT volume label.
- The installer performs no GPT/MBR write, `mkfs`, resize, label change,
  native-root installation, or UEFI-variable mutation.
- A conflicting managed file is refused. Unmanaged data is preserved.
- The loader selects exactly one same-disk non-ESP FAT32 containing
  `/vmunix` and `/boot.cfg`, loads the kernel there, and injects that
  partition's PARTUUID as `boot0`.
- The generated one-section `boot.cfg` selects `rootfs.img`, `data.img`, and
  `swapfile` through `boot0`.
- Firmware fallback/recovery discovery is attempted first. One explicit
  firmware menu/file selection is acceptable; portable automatic Boot entry
  management is later work.
- Native install, whole-disk GPT creation, and filesystem creation are
  explicitly separate later milestones.

## Phase decomposition produced

1. `ws019-p002` exposes read-only disk/GPT/partition/filesystem identity.
2. `ws019-p003` implements read-only `/sbin/diskpart` list/show.
3. WS013 p002 selects the payload FAT and injects `boot0`; WS013 p003 parses
   and translates `boot.cfg`.
4. `ws019-p004` implements the bounded existing-FAT overlay copy transaction.
5. `ws019-p005` accepts that install and boot on disposable QEMU NVMe.
6. WS003 p018 performs one initial Latitude install/boot checkpoint and the
   later frozen repeatability gate.
7. Whole-disk creation and native root remain future p006/p007 work.

## Completion evidence

- The parent WS, master, WS003, WS013, and test indexes point to the same
  staged contract.
- Every initial implementation concern has an owning Phase.
- No unresolved destructive behavior has been placed in a Queue.
- No implementation or disk mutation is claimed by this completed design
  Phase.
