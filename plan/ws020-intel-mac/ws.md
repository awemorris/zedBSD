# WS020: Intel Mac UEFI bring-up

Last updated: 2026-08-31

WSID: `ws020`

Status: active; p001-p003 and p005 automatic work complete, p006 automatic
GPT compatibility repair complete with one provisional physical boot pending,
and p004 final physical acceptance pending

Parent: [master plan](../master.md)

Shared tests: [WS020 test index](tests/README.md)

## Objective

Boot the ordinary amd64 PC/AT zedBSD system on an Intel Mac through an
explicitly UEFI-only disk layout, without turning firmware layout into a
kernel/source-build variant. At the same time, introduce a reusable
Architecture -> Board -> Variant configuration axis so later boards such as
Raspberry Pi 4 can add their own image profiles without inventing another
target hierarchy.

The Apple profile is deliberately a pure UEFI disk: it has a standards-shaped
Protective MBR and GPT, but no compatibility MBR entry or reachable BIOS
loader. Its size and GPT extent are derived from the fixed ESP and payload
layout; the user does not select a target-medium capacity.

## Fixed decisions

- `ZEDBSD_ARCHITECTURE` and `ZEDBSD_BOARD` continue to select compiled kernel
  and driver sources. `ZEDBSD_VARIANT` selects only board-owned image layout.
- The amd64 PC/AT variants and their menu order are:
  - `hybrid`: `UEFI + BIOS (for PC/AT)`
  - `uefi`: `UEFI (for Apple)`
  - `bios`: `BIOS (for PC/AT)`
- `make bootloader` always builds the maintained BIOS and UEFI loader artifacts
  for amd64 PC/AT, regardless of selected image Variant. `vmunix` and every
  compiled loader are identical across the three Variants for an otherwise
  identical configuration.
- There is no disk-capacity configuration field. Variant determines image
  composition only; it does not describe the eventual USB/NVMe capacity.
- UEFI-only retains a standards-shaped Protective MBR, including its `55 aa`
  signature, but contains no executable stage 1, active partition, hybrid FAT
  entry, BIOS boot partition, zedBSD custom BIOS PBR loader, or
  `BOOTZBSD.EXE`. The only nonzero partition record is one non-active `0xee`
  record; the other three records are zero.
- UEFI-only contains an ESP with `EFI/BOOT/BOOTX64.EFI` and a separate FAT32
  payload containing `vmunix`, `zedbsd.cfg`, `rootfs.img`, `data.img`, and
  `swapfile`.
- The fixed UEFI-only artifact is 395,297 512-byte sectors (202,392,064
  bytes). Its primary GPT has `alternate_lba=395296`,
  `last_usable_lba=395263`, and a conventional final 33-sector zero
  reservation. No backup GPT is generated.
- The kernel accepts this intentional primary-only form both when the physical
  medium ends at the declared GPT extent and when the physical medium is
  larger. The larger remainder is ignored unallocated space. A GPT end beyond
  the physical medium or a malformed primary remains an error. Nonzero or
  otherwise noncanonical metadata in the declared final reservation excludes
  the special `intentional primary-only` classification; when the primary GPT
  itself remains fully valid, the ordinary read-only one-copy recovery rule
  still accepts it. The production image checker separately rejects that
  noncanonical source shape.
- Some contemporary raw-image workflows can rewrite that copied primary-only
  form into a complete GPT at the larger medium's physical end while leaving
  the compact Protective-MBR advertisement. P006 makes a valid GPT
  authoritative over that advertisement. The protective entry remains
  mandatory, its extent disagreement is warned, and ordinary CRC, geometry,
  bounds, copy-consistency, and read-only one-copy recovery rules remain in
  force. If the primary header is damaged, recovery examines only the
  PMBR-advertised end and the physical end, accepts one valid candidate (or two
  identical candidates), and rejects contradictions without scanning arbitrary
  LBAs. The source-image layout is unchanged.
- Hybrid keeps its accepted complete GPT plus compatibility-BIOS layout, and
  BIOS-only keeps its legacy MBR layout. Both loader families are nevertheless
  always compiled.
- Secure Boot remains disabled. Signing and Apple-specific NVRAM mutation are
  not part of this WS.

## Variant layouts

| Variant | Partition metadata | Firmware payload | BIOS payload |
| --- | --- | --- | --- |
| `hybrid` | Existing compatibility MBR plus complete GPT | ESP fallback loader | BIOS stage 1/chain/PBR/`BOOTZBSD.EXE` |
| `uefi` | Pure Protective MBR plus fixed primary-only GPT, ESP, and payload FAT32 | ESP fallback loader | none in the image, although BIOS binaries are still built |
| `bios` | Legacy MBR with no GPT or ESP | none in the image, although the UEFI binary is still built | stage 1/PBR/`BOOTZBSD.EXE` and payload FAT |

`BIOS (for PC/AT)` is retained as an independent BIOS regression path and as a
fallback for old PC/AT firmware or tooling that cannot use GPT. The normal
PC/AT default remains the combined UEFI+BIOS profile.

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws020-p001`](phase001-target-variant-config/phase.md) | Completed (revised 2026-08-31) | Capacity selector removed; generic Variant round-trip and three-way compiled-artifact invariance pass with the requested labels/order |
| [`ws020-p002`](phase002-image-layouts/phase.md) | Completed (revised 2026-08-31) | Fixed pure-PMBR primary-only UEFI layout and larger-medium kernel handling pass strict image and GPT host gates |
| [`ws020-p003`](phase003-qemu-acceptance/phase.md) | Complete (`q047`, 2026-08-31) | One fresh uninterrupted `MAC-T020` run passed all six strict positive/negative cells; every positive reached exact `login:` and every immutable source remained unchanged |
| [`ws020-p004`](phase004-physical-bringup/phase.md) | Provisional boot pending | The former `FDC1-A4EF` payload exposed the host-relocated-GPT boundary; p006 automatic repair now passes, so boot the newly frozen `A93F-BBBE` artifact once, then retain the final five-cold-boot gate |
| [`ws020-p005`](phase005-production-uefi-preflight/phase.md) | Completed (`q038`); refreshed `q047` | Current two-partition UEFI-only source passed `MAC-T021` and partition publication ordinary/sanitizer/analyzer gates; exact checked hash `f811a0f5...` is installed |
| [`ws020-p006`](phase006-relocated-physical-gpt/phase.md) | Automatic complete; physical pending | GPT precedence, host/sanitizer/analyzer, relocated/pristine QEMU, exact login, and six-cell gates pass; published image SHA-256 `692160cf...331d`, UUID `A93F-BBBE`, awaits one provisional Intel Mac boot |

## Completion conditions

WS020 is complete when the generic Variant selection is stable, amd64 always
builds both loader families, each selected layout contains only its intended
boot path, the six-cell automatic matrix and the p006 GPT-precedence
compatibility gates pass, and the declared Intel Mac boots the frozen UEFI-only
artifact to a usable login five times in the final campaign. A first successful
physical boot after p006 is enough to continue debugging and implementation;
repetition is deferred to final acceptance.

## Reconsideration boundaries

- Stop and preserve evidence if the Intel Mac requires a nonstandard removable
  path, HFS/APFS blessing, NVRAM entry, signed image, or a Protective-MBR shape
  incompatible with the fixed UEFI-only contract.
- Do not add a compatibility MBR entry or backup GPT silently. Such a change
  requires a new explicit design decision.
- Do not specialize the generic menu hierarchy around one Mac model or make
  Variant change kernel source selection.
