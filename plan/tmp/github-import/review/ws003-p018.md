# WS003 Phase 018: Latitude existing-FAT NVMe overlay installation and boot

Last updated: 2026-08-29

Phase ID: `ws003-p018`

Status: planned; dependency-gated; not ready for a Queue

Parent: [WS003 real-hardware bring-up](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Use the released `/bin/zedinst` from the USB-booted system to place the fixed
overlay installation into an existing ESP plus an explicitly selected existing
FAT32 on the Latitude 5320 internal NVMe, then boot it without formatting or
creating a zedBSD UEFI variable.

## Dependencies

- `ws004-p025`: read-only acceptance of the exact SN740 controller;
- WS013 p002/p003: deterministic first-candidate `zedbsd.cfg` discovery and
  direct parameter translation;
- WS019 p005: the same non-formatting install/boot transaction accepted on
  disposable QEMU NVMe;
- Secure Boot disabled, as already fixed by WS003 policy;
- a user-confirmed existing GPT disk with exactly one usable ESP and one
  disposable/selectable existing FAT32 payload partition.

## Physical procedure boundary

- Present exactly one physical action, its purpose, and the absolute candidate
  image path at a time.
- Before writing, name the SN740 and both PARTUUIDs, display all managed paths,
  and obtain the WS019 confirmation. Never infer a target from enumeration
  order.
- This Phase performs no GPT write, `mkfs`, resize, label change, native-root
  install, managed-file overwrite, or UEFI-variable mutation.
- First try normal firmware discovery of `/EFI/BOOT/BOOTX64.EFI`. If it is not
  offered, ask for exactly one manual firmware boot-menu/file selection. Do not
  add a Boot entry during this Phase.
- Development proceeds after one broadly successful checkpoint. The final
  frozen candidate receives five consecutive successful cold boots; earlier
  repetitions do not block implementation unless the defect is probabilistic.

## Completion conditions

- BR-T49 records the source image, disk/ESP/payload identities, installer
  transaction, managed-file digests, unchanged GPT/labels/NVRAM evidence, and
  the exact firmware selection used.
- Firmware loads the installed fallback `BOOTX64.EFI`; it selects the first
  same-disk payload FAT32 containing `/zedbsd.cfg`, uses `kernel=vmunix`, and
  binds omitted `boot0` to that FAT.
- `/zedbsd.cfg` mounts `rootfs.img` plus writable `data.img`, activates
  `swapfile`, and reaches init/login/root shell from the internal NVMe.
- Unmanaged files on the ESP and payload partition remain byte-identical.
- The final frozen installation passes five consecutive cold boots with no
  NVMe timeout/reset/I/O, payload ambiguity, filesystem, or swap error.

## Reconsideration boundary

Stop and return to planning if the Latitude cannot launch the fallback loader
even through explicit firmware file selection, if the disk lacks the required
existing FAT32 destinations, or if success would require formatting, GPT
mutation, overwriting a conflicting file, or creating a Boot entry.
