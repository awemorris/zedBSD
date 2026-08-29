# WS019: installation and disk administration

Last updated: 2026-08-29

WSID: `ws019`

Status: planned; overlay-only installer-v1 contract fixed

Parent: [master plan](../master.md)

Last verified Phase: `ws019-p001` design contract complete

Resume point: after the WS004 NVMe dependencies, Queue p002 read-only block
administration, then p003--p005 in dependency order. No implementation is
authorized by this plan.

Shared tests: [WS019 test index](tests/README.md)

## Goals

- Install zedBSD from the ordinary USB-booted system with `/bin/zedinst`
  rather than maintaining a separate installer image.
- Make the first install path intentionally small: an existing GPT disk, its
  existing ESP, and one explicitly selected existing FAT32 payload partition.
- Preserve the partition table, filesystem formats, labels, and all unrelated
  files in installer v1.
- Provide a truthful read-only `/sbin/diskpart` before adding any table writer.
- Boot the resulting immutable-root/writable-overlay installation from QEMU
  NVMe and then the Latitude 5320 internal NVMe.
- Keep native-root installation, whole-disk initialization, and formatters as
  later, independently reviewed steps.

## Objective

Create the smallest safe installation path for the current product maturity.
The first installer does not pretend to be a general partition editor and does
not create a dual-boot layout. It verifies an existing GPT/ESP, lets the user
select one existing FAT32 on the same disk, and copies one bounded set of
zedBSD files. A user who does not already have such a destination should keep
using the USB system until the later whole-disk/native work is designed.

## Fixed installer-v1 layout

The two roles are distinct partitions on one existing GPT disk:

```text
ESP (existing FAT32)
  /EFI/BOOT/BOOTX64.EFI

ZEDBSD payload (user-selected existing FAT32)
  /vmunix
  /boot.cfg
  /rootfs.img
  /data.img
  /swapfile
```

`ZEDBSD` is a role name in this contract, not a requirement to change the GPT
partition name or FAT volume label. Installer v1 performs no GPT write, no
`mkfs`, no resize, no label change, and no native-root installation.

## Fixed product and safety decisions

- The commands are `/bin/zedinst` and `/sbin/diskpart`.
- Installation runs from the ordinary USB image; no installer-only image or
  base system is introduced.
- The selected disk must already contain a valid GPT and exactly one usable
  GPT EFI System Partition with a writable FAT32 filesystem.
- The user explicitly selects a different, writable FAT32 partition on the
  same physical disk. Enumeration order is never consent or identity.
- The installer displays the disk identity, ESP PARTUUID, payload PARTUUID,
  capacity/free-space checks, source image identity, and exact managed paths
  before one confirmation.
- Unrelated partitions and files are never touched. A managed destination
  path that is already byte-identical is accepted; a non-identical existing
  object is refused rather than silently overwritten in v1.
- Each new managed file is staged on the destination filesystem, flushed,
  checked for size and digest, and published by same-filesystem rename. A
  failure removes only the unpublished temporary file and reports the exact
  incomplete path.
- Secure Boot remains disabled for the initial Latitude installation.
- Existing one-based partition naming remains authoritative: the first GPT
  partition is `/dev/nvme0n1p1`, never `p0`.
- `/sbin/diskpart` is read-only in this milestone. GPT creation, protective
  MBR/GPT writing, target formatting, raw-offset repair for destructive table
  writes, rescan, and whole-disk confirmation belong to later Phases.

## Boot and payload discovery contract

Installer v1 does not create, reorder, delete, or depend on zedBSD-owned UEFI
`Boot####` variables. It installs the fallback/recovery pathname
`/EFI/BOOT/BOOTX64.EFI`. QEMU and Latitude acceptance first allow firmware
automatic discovery; if that is absent, one explicit firmware menu or
boot-from-file selection is sufficient for this milestone. Portable
unattended fixed-disk boot may add a separately reviewed Boot entry later.

The loader is physically on the ESP while all remaining files are on the
payload FAT32. Therefore loader origin cannot continue to mean `boot0`.
WS013 p002 must:

1. enumerate SimpleFS handles on the same physical GPT disk as the loaded ESP;
2. exclude the ESP and reject non-FAT32 or non-GPT candidates;
3. select exactly one filesystem containing readable `/vmunix` and
   `/boot.cfg`;
4. fail visibly if zero or multiple candidates match;
5. load `/vmunix` from that filesystem; and
6. inject its GPT PARTUUID as explicit `boot0=PARTUUID=...` before translating
   `/boot.cfg`.

The installer preflight applies the same uniqueness rule before copying. No
FAT label, GPT name, extra ESP locator file, or new CPAR handoff ABI is needed.

The initial generated configuration contains one overlay section equivalent
to:

```ini
timeout=5
default=zedBSD

[zedBSD]
rootfs=boot0:rootfs.img
datafs=boot0:data.img
swap=boot0:swapfile
```

WS013 p003 owns its bounded parser/menu and translation to the already
implemented `overlay-root=`, `overlay-data=`, and `swap0=` parameters.

## Source artifact contract

`zedinst` resolves the loader-origin USB boot filesystem through the read-only
administration UAPI, mounts it read-only in a private temporary location, and
uses its verified `BOOTX64.EFI`, amd64 kernel, `rootfs.img`, `data.img`, and
signed `swapfile`. It generates the single-section `/boot.cfg` above rather
than copying mutable configuration from the running overlay. The implementation
Phase must freeze the exact source filenames used by the current image and
verify all sources before writing either destination.

## Required foundations discovered by audit

- The UEFI loader currently opens only its own ESP, loads
  `/VMUNIX.X64`, and treats the ESP FAT serial as implicit `boot0`; it cannot
  boot this two-partition layout without WS013 p002/p003.
- The current UEFI handoff also hard-codes MBR and partition indices. The
  loader/HAL boundary must stop presenting those values as authoritative for
  a GPT boot. Explicit payload PARTUUID selection remains the filesystem
  identity contract.
- A strict GPT enumerator is required to publish partition type, PARTUUID,
  parent identity, and bounds. WS004 p024 owns its block-path acceptance;
  WS019 p002 owns the read-only user-visible query boundary.
- Raw devfs offsets above 4 GiB and duplicate sector submission remain real
  debt, but they do not block this VFS-only, non-table-writing installer. They
  move to the later destructive administration Phase.
- Target-side FAT32 and UFS formatters do not exist and are deliberately not
  required by installer v1.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws019-p001` | [overlay installer-v1 contract](phase001-installer-v1-contract/phase.md) | Completed by design, 2026-08-29 | The existing-ESP/existing-FAT32, no-format, no-Boot-variable contract and Phase map are fixed |
| `ws019-p002` | [read-only block/GPT administration](phase002-readonly-block-gpt-administration/phase.md) | Planned; depends on `ws004-p024` | Stable GPT/disk/partition/filesystem/mount identity is queryable without a mutation surface |
| `ws019-p003` | [read-only `/sbin/diskpart`](phase003-diskpart-readonly/phase.md) | Planned; depends on p002 | List/show the exact GPT disk, ESP, FAT32 candidates, bounds, and stable identities |
| `ws019-p004` | [existing-FAT overlay `/bin/zedinst`](phase004-zedinst-existing-fat-overlay/phase.md) | Planned; depends on p002/p003 and WS013 p002/p003 | Copy and verify only the fixed files without GPT, mkfs, label, or NVRAM mutation |
| `ws019-p005` | [QEMU NVMe overlay-install acceptance](phase005-qemu-nvme-overlay-install/phase.md) | Planned; depends on p004 and `ws004-p024` | A fresh disposable existing-GPT/FAT fixture installs and boots its NVMe overlay |
| `ws019-p006` | Whole-disk GPT creation and filesystem provisioning | Future; not designed | Add destructive initialization only after a separate safety/product review |
| `ws019-p007` | Native-root installation | Future; not designed | Add `rootpart=` installation without changing or weakening p001--p005 |

## Installer-v1 completion conditions

- p002--p005 satisfy their completion conditions without importing external
  base-system code.
- A disposable QEMU NVMe with pre-existing GPT, ESP, and payload FAT32 is
  installed from the ordinary USB system and boots through the installed
  fallback loader into the overlay root.
- Before and after partition tables, filesystem formats/labels, unmanaged
  files, and UEFI variables compare unchanged.
- Zero/multiple ESPs, zero/multiple payload markers, non-FAT32 selection,
  insufficient space, existing conflicting files, copy/flush/verify failure,
  and source/target aliasing fail visibly without a success claim.
- The public guide recommends USB trial use and states that the initial
  installer requires suitable existing filesystems and does not create them.

## Reconsideration boundaries

Return to planning if the Latitude cannot launch the fallback path even by an
explicit firmware selection, if current FAT rename/flush behavior cannot make
publication bounded, if the selected FAT cannot be rediscovered by PARTUUID,
or if implementation would need to format, resize, or rewrite GPT.

## Standards references

- UEFI 2.10 defines `Boot####`/`BootOrder` and fixed-media boot-manager
  behavior:
  <https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html>.
- UEFI 2.10 defines ESP FAT, `EFI/BOOT/BOOT{machine}.EFI`, partition
  discovery, and SimpleFS:
  <https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html>.
