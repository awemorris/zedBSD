# WS003 Phase 018: Latitude NVMe installation and boot

Last updated: 2026-08-29

Phase ID: `ws003-p018`

Status: planned; dependency-gated; not ready for a Queue

Parent: [WS003 real-hardware bring-up](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Use the released `/bin/zedinst` from the USB-booted system to install zedBSD to
the Latitude 5320's internal NVMe, then boot the installed `BOOTX64.EFI` in
both native-root and file-backed overlay-root configurations.

## Dependencies

- `ws004-p025`: read-only acceptance of the exact SN740 controller;
- WS013's UEFI FAT32/LFN and `boot.cfg` implementation;
- WS019's block-administration, `diskpart`, formatting, installer, and QEMU
  NVMe installation acceptance;
- Secure Boot disabled, as already fixed by the WS003 policy.

## Physical procedure boundary

- Present exactly one physical action, its purpose, and the absolute candidate
  image path at a time.
- The destructive installation action must name the internal NVMe explicitly,
  display its stable identity/capacity/current layout, and require the WS019
  whole-disk confirmation. Never infer the target from enumeration order.
- Development proceeds after one broadly successful checkpoint. The final
  frozen candidate receives five consecutive successful cold boots; earlier
  repetitions do not block implementation unless the defect is probabilistic.

## Completion conditions

- BR-T49 records the exact source image, installer transaction, resulting GPT
  identities, `boot.cfg`, and both root-mode boot logs.
- Whole-disk installation completes from the ordinary USB image; no dedicated
  installer image is needed.
- The UEFI firmware loads the installed `BOOTX64.EFI` from the GPT EFI System
  Partition.
- A `boot.cfg` native entry names the UFS partition fixed by WS019 p001 with
  `rootpart=` and reaches init/login/root shell. Under the current two-partition
  proposal this is `/dev/nvme0n1p2`; the FAT32 boot partition is `p1`.
- A separate overlay entry resolves the NVMe boot partition as `boot0`, mounts
  its `rootfs.img` plus writable `data.img`, activates the configured swap, and
  reaches init/login/root shell.
- The final frozen installation passes five consecutive cold boots, with both
  root modes represented and no NVMe timeout, reset, I/O, or filesystem error.

## Reconsideration boundary

Stop and return to planning if the Latitude firmware does not boot the
standards-conforming GPT EFI System Partition, if its NVMe
namespace is not 512-byte LBA, or if completing either mode would require
overwriting a disk other than the explicitly confirmed target.
