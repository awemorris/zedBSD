# WS019: installation and disk administration

Last updated: 2026-08-29

WSID: `ws019`

Status: re-plan required; storage safety contract retained, implementation
language changed to Noct

Parent: [master plan](../master.md)

Last verified Phase: `ws019-p001` design contract complete

Resume point: the user's latest request changes `/bin/zedinst` to a Noct
implementation but ends after `仕様は`, before supplying the replacement
contract. Retain p001's approved existing-GPT/existing-FAT/no-format safety
boundary, but do not Queue the older p002--p005 map until the missing Noct
installer specification is supplied and those Phases are revised.

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
  /zedbsd.cfg
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
WS013 p002/p003 must:

1. enumerate SimpleFS handles on the same physical GPT disk as the loaded ESP;
2. accept same-disk FAT16/FAT32, including the loaded filesystem when it owns
   the configuration;
3. use `/zedbsd.cfg` as the candidate marker;
4. fail visibly if zero candidates match, and warn then use the deterministic
   first candidate if multiple match;
5. read required `kernel=` and load that relative file from the same FAT; and
6. bind an omitted `boot0` and bare image paths to the selected config FAT.

The installer deliberately applies a stricter uniqueness rule before copying:
it refuses another same-disk `/zedbsd.cfg`, ensuring the installed payload is
the loader's first and only candidate. No
FAT label, GPT name, extra ESP locator file, or new CPAR handoff ABI is needed.

The initial generated configuration is direct and contains:

```ini
kernel=vmunix
overlay-root=rootfs.img
overlay-data=data.img
swap0=swapfile
```

WS013 p003 removes the loader-only `kernel=` directive, adds the selected FAT
identity as `boot0` when omitted, prefixes the three bare file values with
`boot0:`, and hands the existing kernel parser a space-separated record. A
future config may instead use direct `rootpart=` for native UFS; no menu or
section syntax is implemented now.

## Source artifact contract

`zedinst` resolves the loader-origin USB boot filesystem through the read-only
administration UAPI, mounts it read-only in a private temporary location, and
uses its verified `BOOTX64.EFI`, amd64 kernel, `rootfs.img`, `data.img`, and
signed `swapfile`. It generates the direct `/zedbsd.cfg` above rather
than copying mutable configuration from the running overlay. The implementation
Phase must freeze the exact source filenames used by the current image and
verify all sources before writing either destination.

## Required foundations discovered by audit

- The UEFI loader currently opens only its own filesystem, loads
  `/VMUNIX.X64`, and treats the ESP FAT serial as implicit `boot0`; it cannot
  boot this two-partition layout without WS013 p002/p003.
- The current UEFI handoff also hard-codes MBR and partition indices. The
  loader/HAL boundary must stop presenting those values as authoritative for
  a GPT boot. The selected config FAT's synthesized `boot0` remains the
  filesystem identity contract.
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
| `ws019-p002` | [read-only block/GPT administration](phase002-readonly-block-gpt-administration/phase.md) | Superseded pending Noct re-plan | Preserve the read-only capability requirement, but revise its implementation boundary after the missing installer contract arrives |
| `ws019-p003` | [read-only `/sbin/diskpart`](phase003-diskpart-readonly/phase.md) | Superseded pending Noct re-plan | Preserve the user-visible read-only inspection goal, but do not assume the old C implementation plan |
| `ws019-p004` | [existing-FAT overlay `/bin/zedinst`](phase004-zedinst-existing-fat-overlay/phase.md) | Superseded; human input required | Rewrite as a Noct Phase after the complete CLI/source/transaction contract and stable installer-source decision are supplied |
| `ws019-p005` | [QEMU NVMe overlay-install acceptance](phase005-qemu-nvme-overlay-install/phase.md) | Superseded pending Noct re-plan | Rebuild the acceptance Phase around the revised Noct installer without weakening p001's non-formatting safety contract |
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
- Zero/multiple ESPs, zero payload markers, an already-present second config,
  non-FAT32 selection,
  insufficient space, existing conflicting files, copy/flush/verify failure,
  and source/target aliasing fail visibly without a success claim.
- The public guide recommends USB trial use and states that the initial
  installer requires suitable existing filesystems and does not create them.

## Reconsideration boundaries

Return to planning if the Latitude cannot launch the fallback path even by an
explicit firmware selection, if current FAT rename/flush behavior cannot make
publication bounded, if the selected FAT cannot be rediscovered by PARTUUID,
or if implementation would need to format, resize, or rewrite GPT.

The ordinary USB runtime currently uses `DATA.IMG` as its writable overlay
upper and `SWAPFILE` as active swap. They are not stable installation inputs
and must never be copied live. Before p004 enters a Queue, choose either unused
immutable installer templates (recommended) or a separately designed
target-generation contract.

## Standards references

- UEFI 2.10 defines `Boot####`/`BootOrder` and fixed-media boot-manager
  behavior:
  <https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html>.
- UEFI 2.10 defines ESP FAT, `EFI/BOOT/BOOT{machine}.EFI`, partition
  discovery, and SimpleFS:
  <https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html>.
