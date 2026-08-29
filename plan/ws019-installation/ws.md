# WS019: installation and disk administration

Last updated: 2026-08-29

WSID: `ws019`

Status: planning; `ws019-p001` records the remaining installer-v1 decisions

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: finish `ws019-p001`, then propose the block-administration and
`diskpart` foundation as a finite Queue. No implementation is authorized by
this plan.

Shared tests: [WS019 test index](tests/README.md)

## Goals

- Install zedBSD from the ordinary USB-booted system with `/bin/zedinst` rather
  than maintaining a separate installer image.
- Provide a small, truthful `/sbin/diskpart` for GPT inspection and the
  deliberately limited zedBSD whole-disk layout.
- Support an expert existing-partition path and a simple, explicit erase-the-
  whole-disk UEFI path without offering resize, move, or dual-boot workflows.
- Produce installations that QEMU can boot from NVMe in both native UFS-root
  and immutable-root/writable-overlay modes.
- Make USB boot the documented recommendation for trying zedBSD without
  changing internal storage.

## Objective

Create the smallest safe installation path for the current product maturity.
The guided default is intentionally not a general partition editor: users may
either supply already selected partitions or explicitly erase one whole disk
and install a fixed zedBSD UEFI layout. This restriction reduces the chance
that a novice treats an immature dual-boot workflow as safe and destroys an
existing system. A user who only wants to evaluate zedBSD should continue to
boot and use the USB image.

## Fixed v1 decisions

- The commands are `/bin/zedinst` and `/sbin/diskpart`.
- Installation runs from the ordinary bootable image; there is no separate
  installer image or installer-only base system.
- Partition-table editing supports GPT only. Whole-disk initialization writes
  the required protective MBR plus primary and backup GPT headers and entry
  arrays with checked CRC32. Editing a legacy MBR, hybrid MBR, resize, move,
  recovery, and dual-boot automation are outside v1.
- The whole-disk UEFI layout starts with a standard GPT EFI System Partition
  containing FAT32 and uses zedBSD-owned GPT partition type GUIDs selected by
  p001 for native UFS and any later raw swap/data roles.
- Installer v1 always writes `EFI/BOOT/BOOTX64.EFI` to the ESP. Whether it also
  creates one zedBSD firmware boot variable remains an explicit p001 decision;
  it never reorders or deletes unrelated firmware entries implicitly.
- Existing one-based partition naming is retained. The first partition of
  `nvme0n1` is `/dev/nvme0n1p1`; `p0` continues to mean the raw device only in
  UEFI device-path terminology and is not a zedBSD partition node.
- Existing-partition mode accepts only explicitly selected, already enumerated
  GPT partitions with stable PARTUUIDs in v1. It never converts an MBR disk.
- `diskpart` without a destructive verb is read-only. Whole-disk initialization
  requires an explicit raw-disk target, a stable identity/capacity display,
  and typing an exact erase confirmation; it never selects a disk implicitly.
- `zedinst` offers only existing-partition and whole-disk modes, shows one final
  transaction plan before writing, and propagates the first real failure.
- Secure Boot remains disabled for the initial Latitude installation.

## Required foundations discovered by audit

- Raw devfs block offsets currently truncate above 4 GiB and must become
  64-bit before any NVMe installer write. The same raw write path currently
  submits each completed sector write twice; p002 must remove that duplicate
  submission and lock it down with a focused regression.
- The target lacks a block-information/admin UAPI for capacity, logical block
  size, whole-disk/partition identity, parent ranges, exclusive destructive
  mutation, and safe partition rescan.
- There is no complete GPT partition enumerator or target-side GPT writer. The
  read-only parser must validate both headers, both entry-array CRCs, usable
  ranges, overlaps, type/unique GUIDs, and bounded names before partitions are
  published; `diskpart` must generate the same on-disk invariants.
- No target-side FAT32 or UFS formatter exists. Runtime mount/write support is
  not itself a formatter.
- The UEFI loader currently consumes EFI LoadOptions or its compiled fallback
  and does not yet read `boot.cfg`; WS013 owns that prerequisite.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws019-p001` | [installer v1 contract](phase001-installer-v1-contract/phase.md) | Planned; design decisions open | Freeze layouts, existing-partition mutation semantics, formatting/provisioning, confirmation, recovery, and test matrix |
| `ws019-p002` | Block administration UAPI and correct 64-bit raw I/O | Future; depends on p001 | Safe size/identity/parent query, exclusive destructive claim, rescan, >4-GiB raw access, and exactly-once writes |
| `ws019-p003` | `/sbin/diskpart` GPT v1 | Future; depends on p002 and `ws004-p024` | Read/list plus explicitly confirmed fixed whole-disk protective-MBR/GPT initialization |
| `ws019-p004` | Target FAT32/UFS provisioning | Future; depends on p001/p002 | Format or provision each selected destination with verified bounds and flush |
| `ws019-p005` | `/bin/zedinst` existing-partition mode | Future; depends on p003/p004/WS013 | Install without changing the partition table, within the exact approved mutation contract |
| `ws019-p006` | `/bin/zedinst` whole-disk mode | Future; depends on p003/p004/p005 | Erase one explicit disk, create the fixed layout, install and verify every artifact |
| `ws019-p007` | QEMU NVMe installation acceptance | Future; depends on p006 and `ws004-p024` | Fresh install boots native root and overlay root from QEMU NVMe |

Only p001 has a P book now. Later P books are extracted after its product and
safety decisions are frozen; this prevents unresolved destructive behavior
from entering a Queue.

## WS completion conditions

- The safe block-administration contract, `diskpart`, destination formatting,
  and both `zedinst` modes are implemented without external base-system code.
- A disposable QEMU NVMe can be initialized and installed from the ordinary
  amd64 USB image, then booted through installed `BOOTX64.EFI` with both native
  `rootpart` and NVMe-boot-partition overlay configurations.
- Failure injection covers preflight, table write/flush/rescan, format,
  artifact copy, `boot.cfg`, and verification boundaries without ever claiming
  an incomplete installation succeeded.
- The public guide recommends USB trial use, states that v1 whole-disk mode
  destroys the target and cannot coexist with another OS, and gives a recovery
  path.

## Reconsideration boundaries

Return to planning if the Latitude cannot UEFI-boot the GPT/ESP layout, if
the installer needs to preserve or resize an unknown filesystem, if safe
same-boot rescan cannot exclude mounted/swap descendants, or if a useful
native root requires an unsupported online UFS grow operation.

## Standards references

- UEFI 2.10 defines the protective MBR, redundant GPT headers and entry
  arrays, CRC checks, and the EFI System Partition type GUID:
  <https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html#gpt-disk-layout>.
- UEFI numbers GPT partition device-path entries starting at one:
  <https://uefi.org/specs/UEFI/2.10/10_Protocols_Device_Path_Protocol.html#hard-drive>.
