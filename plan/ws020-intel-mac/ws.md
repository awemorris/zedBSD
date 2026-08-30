# WS020: Intel Mac UEFI bring-up

Last updated: 2026-08-30

WSID: `ws020`

Status: in progress; p001-p002 completed, next executable Phase is p003

Parent: [master plan](../master.md)

Shared tests: [WS020 test index](tests/README.md)

## Objective

Boot the ordinary amd64 PC/AT zedBSD system on an Intel Mac through an
explicitly UEFI-only disk layout, without turning firmware layout into a
kernel/source-build variant.  At the same time, introduce a reusable
Architecture -> Board -> Variant configuration axis and a declared target-media
capacity so later boards such as Raspberry Pi 4 can select their own image
profiles without inventing another target hierarchy.

The UEFI-only artifact is deliberately a compact flash image.  Its primary GPT
declares the selected target medium's final LBA, while the file itself ends
after the populated front partitions and contains no backup GPT.  Writing it to
a medium whose exact capacity matches the selected value supplies the otherwise
sparse/unwritten tail.  QEMU acceptance materializes that tail as a disposable
sparse file before booting it.

## Fixed decisions

- `ZEDBSD_ARCHITECTURE` and `ZEDBSD_BOARD` continue to select compiled kernel
  and driver sources.  `ZEDBSD_VARIANT` selects only board-owned image layout.
- The amd64 PC/AT variants are `hybrid`, `bios`, and `uefi`; their menu labels
  are `Hybrid (BIOS+UEFI)`, `BIOS-only`, and `UEFI-only (for Apple)`.
- `make bootloader` always builds the maintained BIOS and UEFI loader artifacts
  for amd64 PC/AT, regardless of selected image variant.  `vmunix` is identical
  across the three variants for an otherwise identical configuration.
- The declared target-medium choices are exactly 2, 4, 8, 16, 32, 64, 128,
  and 256 GiB.  They are stored independently of platform so another board may
  interpret the same generic setting.  An older configuration without this
  field defaults to the smallest supported capacity, 2 GiB.
- UEFI-only retains a standards-shaped protective MBR, including its `55 aa`
  signature, but contains no executable stage 1, active partition, hybrid FAT
  entry, BIOS boot partition, zedBSD custom BIOS PBR loader, or
  `BOOTZBSD.EXE`.  Formatter-owned FAT32 BPB/VBR bytes remain but provide no
  reachable zedBSD BIOS boot path.
- UEFI-only contains an ESP with `EFI/BOOT/BOOTX64.EFI` and a separate FAT32
  payload containing `vmunix`, `zedbsd.cfg`, `rootfs.img`, `data.img`, and
  `swapfile`.
- A primary-only GPT is accepted only when its header/table are fully valid,
  its declared alternate LBA equals the actual materialized medium's last LBA,
  all partitions fit, and the absent backup region is zero.  A present but
  corrupt or contradictory backup is not reclassified as intentionally absent.
- Hybrid keeps its accepted 203,423,744-byte complete-GPT artifact and BIOS
  keeps its 135,266,304-byte legacy-MBR artifact.  Their selected GiB value is
  a validated target-media constraint only.  Only UEFI-only encodes the exact
  selected last LBA and remains a compact 202,375,168-byte primary-only image.
- Writing compact UEFI-only to reused media must include explicit zeroing of
  the selected medium's final 33 sectors; copying the short file alone cannot
  erase a stale backup GPT at the physical end.
- Secure Boot remains disabled.  Signing and Apple-specific NVRAM mutation are
  not part of this WS.

## Variant layouts

| Variant | Partition metadata | Firmware payload | BIOS payload |
| --- | --- | --- | --- |
| `hybrid` | Existing hybrid MBR plus GPT layout; preserve the already accepted BIOS+UEFI behavior | ESP fallback loader | BIOS stage 1/chain/PBR/`BOOTZBSD.EXE` |
| `bios` | Legacy MBR layout with no GPT or ESP | none in the image, although the UEFI binary is still built | stage 1/PBR/`BOOTZBSD.EXE` and payload FAT |
| `uefi` | Protective MBR plus primary-only GPT, ESP, and payload FAT32; no BIOS boot partition | ESP fallback loader | none in the image, although BIOS binaries are still built |

The selected capacity is an image-format input, not permission to enlarge a
partition.  ESP and payload geometry remain bounded near the front of the
medium; the remaining LBAs are unallocated.  For a format with no on-disk
whole-medium size field, the builder must state explicitly whether the setting
is only an acceptance-media constraint rather than silently changing a
partition.

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws020-p001`](phase001-target-variant-config/phase.md) | Completed (2026-08-30) | Generic Variant/capacity round-trip and validation pass; fresh Variant/capacity builds have identical kernel, loader, object, and compile-contract results |
| [`ws020-p002`](phase002-image-layouts/phase.md) | Completed (2026-08-30) | Three strict image profiles pass; UEFI-only encodes all eight declared capacities in a compact primary-only GPT while Hybrid/BIOS bytes remain capacity-invariant |
| [`ws020-p003`](phase003-qemu-acceptance/phase.md) | Planned after p002 | SeaBIOS/OVMF positive and negative matrix passes at all declared capacities using materialized sparse media |
| [`ws020-p004`](phase004-physical-bringup/phase.md) | Planned after p003; physical checkpoint | One Intel Mac UEFI-only boot reaches login, then the frozen artifact passes the final five-run campaign |

## Completion conditions

WS020 is complete when the generic Variant/capacity selections are stable,
amd64 always builds both loader families, each selected layout contains only
its intended boot path, the full automatic matrix passes, and the declared
Intel Mac boots the frozen UEFI-only artifact to a usable login five times in
the final campaign.  A first successful physical boot is enough to continue
debugging and implementation; repetition is deferred to the final acceptance
campaign.

## Reconsideration boundaries

- Stop physical acceptance if the actual device sector count does not exactly
  match the selected capacity; record it rather than publishing a knowingly
  mismatched GPT.  A later exact-sector/custom-size extension can be planned.
- Stop and preserve evidence if the Intel Mac requires a nonstandard removable
  path, HFS/APFS blessing, NVRAM entry, signed image, or a protective-MBR shape
  incompatible with the fixed UEFI-only contract.
- Do not remove the protective-MBR signature merely to make the image appear
  non-BIOS; BIOS bootability is removed through contents and partition layout.
- Do not specialize the generic menu hierarchy around one Mac model or make
  Variant change kernel source selection.
