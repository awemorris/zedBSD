# WS013 Phase 002: UEFI ESP-to-payload discovery

Last updated: 2026-08-29

Phase ID: `ws013-p002`

Status: planned; dependency-gated

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Allow `BOOTX64.EFI` on an ESP to deterministically locate and load `/vmunix`
from the separate existing FAT32 selected by installer v1, then identify that
payload filesystem to the kernel as explicit `boot0`.

## Dependencies

- the strict GPT/PARTUUID publication accepted by `ws004-p024`;
- the q015 textual boot-parameter and `boot0=PARTUUID=...` contract;
- no dependency on UEFI NVRAM mutation.

## Selection contract

1. Open the loaded image's DeviceHandle and verify it is a GPT ESP on a FAT
   filesystem.
2. Enumerate UEFI SimpleFS handles and retain only GPT partitions on the same
   physical device path prefix as that ESP.
3. Exclude the ESP, require FAT32, and test for readable root files `/vmunix`
   and `/boot.cfg`.
4. Accept exactly one match. Zero, multiple, cross-disk, non-GPT, malformed
   device path, zero/duplicate GUID, and media-change cases fail visibly.
5. Read the selected Hard Drive Media Device Path's GPT unique partition GUID,
   format it as `boot0=PARTUUID=...`, and preserve it through p003 parameter
   translation.
6. Load and validate `/vmunix` from the selected payload, not from the ESP.

`ZEDBSD` is not a required GPT name or FAT volume label. File markers are safe
only because same-disk scope and exact-one matching are mandatory.

## Compatibility and handoff

- Existing single-filesystem generated media may retain the current
  `/VMUNIX.X64` plus implicit loader-origin `boot0` fallback when no installed
  two-volume configuration is requested.
- The UEFI handoff must stop hard-coding an MBR scheme and fixed partition
  indices as if they described this GPT boot. The internal ABI may mark those
  legacy fields unavailable or truthfully extend their interpretation, but
  payload identity remains the explicit textual PARTUUID selector.
- Every handle, pool allocation, root/file handle, and failure path is closed
  or freed before `ExitBootServices()`.

## Completion conditions

- Host fixtures cover device-path prefix comparison, GPT GUID formatting,
  exact-one selection, FAT32 validation, marker errors, allocation cleanup,
  and parameter-length overflow.
- OVMF loads the production kernel from a separate same-disk payload FAT32 and
  reports the expected `boot0` PARTUUID.
- An auxiliary disk with identical marker filenames is ignored, while a
  second same-disk candidate is an ambiguity error.
- The ordinary single-partition USB image still boots.

## Reconsideration boundary

Return to planning if the firmware does not expose enough device-path or
SimpleFS identity to prove same-disk membership, or if preserving the existing
single-volume path would make the installed path ambiguous.
