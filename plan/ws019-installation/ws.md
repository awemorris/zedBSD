# WS019: installation and disk administration

Last updated: 2026-09-05

WSID: `ws019`

Status: active; Noct 2.0.1 is integrated and the temporary implementation-
language block is released. The user selected target-side creation through new
`mkfs` and `mkswap` commands rather than installer templates. P002 is
selected as pending in proposed q076; p003, p008, p009, p004, and p005 follow.

Parent: [master plan](../master.md)

Last verified Phase: `ws019-p001` design contract complete

Resume point: WS011 p007/p009 completed in q075. Approve proposed q076 for
p002's read-only storage UAPI only. P003 `/sbin/diskpart`, p008 `/sbin/mkfs`, p009
`/sbin/mkswap`, p004 Noct `/bin/zedinst`, and p005 QEMU acceptance in that
dependency order require later Queue selection.

Shared tests: [WS019 test index](tests/README.md)

## Goals

- Install zedBSD from the ordinary USB-booted system with `/bin/zedinst`
  rather than maintaining a separate installer image.
- Make the first install path intentionally small: an existing GPT disk, its
  existing ESP, and one explicitly selected existing FAT32 payload partition.
- Preserve partition-table and existing partition filesystem formats, labels,
  and all unrelated files in installer v1. Newly created `data.img` and
  `swapfile` are formatted as files inside the selected FAT32.
- Provide a truthful read-only `/sbin/diskpart` before adding any table writer.
- Boot the resulting immutable-root/writable-overlay installation from QEMU
  NVMe and then the Latitude 5320 internal NVMe.
- Keep native-root installation, whole-disk initialization, and partition
  filesystem formatting as later, independently reviewed steps. Installer v1
  does create the two file-backed runtime objects with target-side tools.

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
partition formatting, no resize, no label change, and no native-root
installation. It does invoke target-side `mkfs` for the new UFS `data.img` and
`mkswap` for the new swap file.

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
- Each copied or generated managed file is staged on the destination filesystem, flushed,
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
WS013 p002/p003 completed this discovery/parsing contract in q031:

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
uses its verified `BOOTX64.EFI`, amd64 kernel, and read-only `rootfs.img`.
It creates the destination `data.img` and `swapfile` at explicitly bounded
sizes and invokes the target `/sbin/mkfs` and `/sbin/mkswap`; it never copies
the running overlay upper or active swap. It generates the direct
`/zedbsd.cfg` above rather than copying mutable live configuration.

## Foundations and current gaps

- WS013 p002/p003 completed same-disk config-volume discovery and configured
  kernel loading in q031; the former own-filesystem-only `/VMUNIX.X64` path is
  historical, not an outstanding prerequisite. Q032 completed the configured
  BIOS paths. P002 must expose the retained selected boot/config filesystem
  truthfully without confusing it with the firmware-loaded ESP.
- The selected config FAT's synthesized `boot0` remains the filesystem
  identity contract. Current versioned boot metadata distinguishes GPT from
  legacy one-based MBR indices; p002 must not infer missing physical-loader
  provenance from an MBR index, enumeration order, or the running root.
- A strict GPT enumerator is required to publish partition type, PARTUUID,
  parent identity, and bounds. WS004 p024 owns its block-path acceptance;
  WS019 p002 owns the read-only user-visible query boundary.
- Raw devfs offsets above 4 GiB and duplicate sector submission remain real
  debt, but they do not block this VFS-only, non-table-writing installer. They
  move to the later destructive administration Phase.
- Target-side UFS-in-file and swap-in-file initializers do not yet exist and
  are explicit installer-v1 prerequisites in p008/p009. FAT32 formatting and
  block-device formatting remain outside installer v1.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws019-p001` | [overlay installer-v1 contract](phase001-installer-v1-contract/phase.md) | Completed by design, 2026-08-29 | The existing-ESP/existing-FAT32, no-format, no-Boot-variable contract and Phase map are fixed |
| `ws019-p002` | [read-only block/GPT administration](phase002-readonly-block-gpt-administration/phase.md) | Pending in proposed q076 | Versioned read-only snapshot, full GPT metadata, use/boot provenance and opened-object change detection; no implementation before approval |
| `ws019-p003` | [read-only `/sbin/diskpart`](phase003-diskpart-readonly/phase.md) | Planned; follows p002 | Implement the frozen read-only inspection grammar after p002 |
| `ws019-p004` | [existing-FAT overlay `/bin/zedinst`](phase004-zedinst-existing-fat-overlay/phase.md) | Planned Noct implementation; follows p002/p003/p008/p009 | Copy immutable boot/root artifacts and create fresh data/swap files through target commands |
| `ws019-p005` | [QEMU NVMe overlay-install acceptance](phase005-qemu-nvme-overlay-install/phase.md) | Planned; follows p002--p004 and p008/p009 | Run the frozen non-partition-formatting QEMU NVMe acceptance |
| `ws019-p006` | Whole-disk GPT creation and filesystem provisioning | Future; not designed | Add destructive initialization only after a separate safety/product review |
| `ws019-p007` | Native-root installation | Future; not designed | Add `rootpart=` installation without changing or weakening p001--p005 |
| `ws019-p008` | [target `/sbin/mkfs`](phase008-target-mkfs/phase.md) | Planned; follows p002 | Create a bounded UFS1 filesystem in a newly created regular file without formatting its containing partition |
| `ws019-p009` | [target `/sbin/mkswap`](phase009-target-mkswap/phase.md) | Planned; follows p002 | Create the existing ZEDSWAP2 format in a newly created regular file with bounded size and publication |

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
  installer requires suitable existing partition filesystems, does not format
  them, and creates only the contained UFS `data.img` and ZEDSWAP2 `swapfile`.

## Reconsideration boundaries

Return to planning if the Latitude cannot launch the fallback path even by an
explicit firmware selection, if current FAT rename/flush behavior cannot make
publication bounded, if the selected FAT cannot be rediscovered by PARTUUID,
or if implementation would need to format, resize, or rewrite GPT.

The ordinary USB runtime currently uses `DATA.IMG` as its writable overlay
upper and `SWAPFILE` as active swap. They are not installation inputs and must
never be copied live. The selected contract is target generation through
p008/p009; immutable installer templates are not part of this WS.

## Standards references

- UEFI 2.10 defines `Boot####`/`BootOrder` and fixed-media boot-manager
  behavior:
  <https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html>.
- UEFI 2.10 defines ESP FAT, `EFI/BOOT/BOOT{machine}.EFI`, partition
  discovery, and SimpleFS:
  <https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html>.
