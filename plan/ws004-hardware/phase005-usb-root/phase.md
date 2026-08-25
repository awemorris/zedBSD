# WS004 Phase 005: stable USB boot/root continuity

Last updated: 2026-08-25

Phase ID: `ws004-p005`

Status: partial; identity complete, writable-root acceptance uncleared

Acceptance disposition: **Uncleared**

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Boot the amd64 PC/AT image as a QEMU xHCI USB mass-storage device, enumerate it
through the native USB stack, identify the firmware-selected boot medium without
depending on kernel discovery order, mount its boot/root content, and reach a
usable login with bounded missing-media failure.

This Phase owns the QEMU U0--U5 software milestone. Physical Latitude evidence
remains in WS003.

## Baseline and finding

`ws004-p004` starts q35 xHCI through MSI-X, enumerates USB storage, completes a
real media read, and survives disconnect/reconnect. It does not boot from that
device. The current boot path has two independent blockers:

1. `kernel_entry()` treats an empty platform boot-device table as fatal before
   deferred xHCI root probing can register USB storage.
2. PC/AT VFS selection maps the firmware BIOS drive number back to an IDE disk.
   A BIOS number such as `0x80` is not a durable identity after native USB/IDE
   enumeration and UEFI does not provide an equivalent kernel device number.

The image builder already writes a non-zero MBR disk signature and GPT unique
partition GUIDs. The partition/block-identity layer can parse those values, but
the PC/AT loader-to-kernel handoff does not carry either identity.

## Fixed decision

Use the existing block-identity selector language for the boot partition:

- `boot=UUID=...` selects the boot filesystem by filesystem UUID;
- `boot=PARTUUID=...` selects it by the existing MBR/GPT partition UUID;
- the standard FAT image is handed off automatically as `UUID=<volume-id>`;
- an absent selector is accepted only for legacy compatibility and retains the
  current IDE/BIOS fallback, never for stable USB-root acceptance.

The BIOS and UEFI loaders obtain the FAT volume ID from the filesystem boot
sector. The amd64 HAL validates it and exposes the canonical existing selector
to the kernel. The kernel first enumerates all candidate block devices, then
uses the existing block-identity resolver to select exactly one boot partition.
Zero matches fail visibly after a bounded probe; multiple matches fail as
ambiguous. Discovery order is not a tie breaker.

`PARTUUID` remains available for installers that prefer partition identity:
MBR already represents it as disk signature plus partition number, while GPT
uses its partition GUID. No new tagged MBR/GPT identity is introduced. The
common 24-byte `boot_handoff` prefix remains intact.

## Human decision

Approved on 2026-08-25: the boot partition uses the same selector vocabulary as
root selection. Both `UUID=` and `PARTUUID=` are supported; the standard FAT
loader handoff uses `UUID=` because that filesystem is deliberately stable for
installer and embedded-image use. MBR signature is only an ingredient of an
MBR `PARTUUID`, not a separate boot ABI.

## Scope after approval

- Define and validate the PC/AT kernel handoff extension without changing other
  platform handoff layouts.
- Extend legacy BIOS and x64 UEFI loaders to provide boot identity.
- Allow PC/AT platform discovery to continue with no IDE disk and wait boundedly
  for deferred xHCI storage discovery.
- Separate platform block enumeration from boot-medium selection.
- Match MBR/GPT identity after partition discovery and reject zero/ambiguous
  matches with actionable diagnostics.
- Exercise the existing FAT/loop-root layout from USB and preserve explicit
  `root=` selection for the root filesystem inside/after the boot medium.
- Add bounded missing/delayed-media behavior and reorder tests.

## Non-goals

- Physical Latitude completion, Secure Boot, USB controller firmware quirks, or
  persistent naming of arbitrary non-boot removable media.
- Replacing filesystem UUID/PARTUUID root selection.
- Treating USB topology, `/dev/sdN`, or BIOS drive number as stable identity.
- NVMe root or a general platform-independent boot-policy language.

## Expected files and subsystems

- `bootloader/include/amd64-handoff.h`
- PC/AT BIOS loader handoff assembly and `bootloader/uefi/bootx64.c`
- `include/kern/boot.h` or a new PC/AT-specific boot-extension header
- `src/hal/amd64/bsp-pcat/boot.c`
- `src/kern/entry.c`, `src/kern/pcat/platform.c`, and `src/kern/vfs.c`
- MBR/GPT identity helpers and WS004 focused/QEMU fixtures

## Ordered work packages

- [x] Audit current loader, HAL, device-discovery, partition, and VFS selection
      paths.
- [x] Identify an existing stable on-disk identity for BIOS and UEFI images.
- [x] Obtain the boot-identity ABI decision.
- [ ] Add the remaining malformed-handoff focused fixture; valid, absent, and
      duplicate runtime evidence passes.
- [x] Implement identity handoff in BIOS and UEFI loaders.
- [x] Reorder/bound PC/AT USB block discovery and remove the IDE-only fatal path.
- [x] Implement exact boot-partition matching and diagnostics.
- [x] Clear the HW-T11 writable-overlay and clean-reboot cases through
      `ws004-p006` and `ws004-p007`; identity/discovery subcases pass.
- [x] Record identity/discovery evidence and remaining physical/bug handoff.

## Completion conditions

- The production amd64 BIOS and UEFI loaders boot from QEMU USB mass storage
  through q35 `qemu-xhci` and reach `login:`.
- The selected boot partition is stable when another IDE or USB disk is attached
  first or enumerated first.
- An absent, delayed beyond timeout, or duplicate-identity boot medium fails
  visibly and without an infinite wait.
- Sustained reads plus a disposable bounded write/sync/readback test complete
  without reset, console/storage errors, or corruption.
- Focused host fixtures, relevant amd64/i386 configured builds, `make -j16`, and
  `git diff --check` pass. The aggregate `make check` target is not used.

## Result

The BIOS and OVMF/UEFI production paths boot the amd64 image as the only xHCI
USB disk through `login:`. FAT UUID handoff, exact existing-resolver selection,
USB/IDE reorder cases, duplicate rejection, five-second missing-media failure,
and delayed replacement inside that bound pass.

The earlier retained-checksum observation alone did not clear write acceptance.
Subsequent ordinary USB boot/login reproduced `loop1` writes failing with
`EIO` at blocks 48 and 56. A usable prompt and matching contents elsewhere are
insufficient while the writable overlay reports storage errors. `ws004-p006`
then recorded three fresh writable USB boots, explicit copies, cold retained
readback, IDE control, and bounded read-only failure injection. User acceptance
subsequently reproduced EIO at blocks 32 and 40 with SMP=4 and NE2000, so that
sample does not clear writable-root acceptance. `ws004-p007`
completed three consecutive BIOS/IDE reboots and a combined q35/xHCI USB
reboot. Identity/discovery remains complete, but HW-T11 and this Phase remain
Uncleared pending `ws004-p006`. Detailed commands and
observations are in
[qemu-usb-root-evidence.md](../tests/qemu-usb-root-evidence.md) and
[qemu-warm-reset-evidence.md](../tests/qemu-warm-reset-evidence.md).

The run also fixed an ESP/backup-GPT overlap in the active host image builder
and added a Noct checker bound. The aggregate `make check` target was not used.

## Follow-up

The malformed-handoff host fixture remains useful defense-in-depth work, and
USB throughput remains performance debt. The writable-overlay EIO blocks the
QEMU runtime milestone. Physical Latitude U0--U5 acceptance remains in WS003.
