# Queue: UEFI `zedbsd.cfg` boot path

Last updated: 2026-08-30

QID: `q031`

Queue status: completed

Queue finished: **Yes**

Authorization: after reviewing the current `BOOTX64.EFI` behavior, the user
fixed the `zedbsd.cfg` discovery, kernel directive, direct-parameter,
shorthand, native-root, missing-config, and LoadOptions policy and explicitly
requested removal of the dead startup path on 2026-08-30.

Timebox: no fixed wall-clock limit. Complete or honestly mark `uncleared` each
finite software Phase. No physical Latitude action is requested in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q030](queue-q030.md)

## Purpose

Replace the hard-coded amd64 UEFI boot contract with one simple editable
`/zedbsd.cfg` on a same-disk FAT16/FAT32, while retaining the existing textual
kernel-parameter ABI. Remove the proven-dead kernel startup menu and audit the
remaining boot path before adding more policy.

Read-only `diskpart` and `zedinst` move to the next dependency-ready Queue.
The installer source-image stability decision remains open and is not hidden
inside this bootloader work.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws013-p002` | [Phase](ws013-containers/phase002-uefi-payload-discovery/phase.md) | completed | Search same-physical-disk FAT16/FAT32 filesystems for `/zedbsd.cfg`; zero is fatal, multiple warns and uses the deterministic first candidate |
| 2 | `ws013-p003` | [Phase](ws013-containers/phase003-uefi-boot-config-menu/phase.md) | completed | Parse required `kernel=`, normalize direct kernel parameters and shorthand, load the selected-FAT kernel, and boot overlay or native UFS with LoadOptions ignored |
| 3 | `ws013-p004` | [Phase](ws013-containers/phase004-boot-path-dead-source-audit/phase.md) | completed | Complete the post-p003 inventory without guessing that test-only code is dead, retaining or deleting each candidate from evidence |

## Dependency order

```text
q030 strict GPT/PARTUUID/NVMe
        |
        `--> ws013-p002 discovery --> ws013-p003 cfg/load/handoff
                                      `--> ws013-p004 final dead-path audit

future q032: ws019-p002/p003 read-only administration
        `--> source-template decision --> ws019-p004/p005 installer
```

## Frozen behavior

- Search only UEFI SimpleFS handles on the same physical device as the loaded
  `BOOTX64.EFI`; FAT16 and FAT32 are supported, FAT12 is not.
- Check the loaded filesystem first, then retain UEFI handle-enumeration order.
  Scan every eligible handle to count candidates. Zero candidates prints that
  `/zedbsd.cfg` was not found and stops. More than one prints a warning and
  uses the first candidate.
- `kernel=PATH` is one required loader-only directive. It is removed from the
  kernel parameter text and names a volume-root-relative file on the selected
  config FAT; one leading slash is accepted. No device selector, empty
  component, `.` or `..` is allowed.
- Every other nonempty line is one existing textual kernel parameter. The
  loader converts bounded ASCII LF/CRLF lines to a space-separated record.
  There are no sections, menu, timeout, comments, quoting, or escaping.
- Both root modes are supported: `rootpart=` selects native UFS, while
  `overlay-root=` plus `overlay-data=` selects the existing overlay path.
- If `boot0=` is absent, the loader inserts the selected config FAT's UUID.
  An explicit `boot0=` is preserved, so an advanced configuration may use the
  config/kernel FAT only as the loader source and bind `boot0` elsewhere.
- Bare `overlay-root`, `overlay-data`, and `swapN` file values receive the
  `boot0:` prefix. An existing `boot0:`--`boot3:` prefix is retained. A raw
  swap selector must be explicit as `/dev/...`, `UUID=`, `LABEL=`,
  `PARTUUID=`, or `PARTLABEL=` and is not prefixed.
- On this configured path, UEFI LoadOptions are ignored. Missing or invalid
  `zedbsd.cfg`, missing/invalid kernel, parameter duplication, or expanded text
  beyond the existing 3071-byte ceiling fails visibly; there is no hard-coded
  fallback configuration.
- `/zedbsd.cfg` is the UEFI configuration name. The deleted kernel-internal
  legacy `BOOT.CFG`/`ZEDBSD.CFG` startup menu is not revived or reused.

## Execution rules

- Do not inspect or modify `.internal/` or `userland/noct/NoctLang`.
- Preserve unrelated work and stage only q031 files at Phase boundaries.
- Use `make -j16`, focused WS013 fixtures, and `qemu-system-x86_64`; do not use
  `make check`.
- Run ordinary, sanitizer, and analyzer variants for the pure discovery and
  configuration helpers where supported.
- After each completed Phase, synchronize P/W/M/Q, commit `WIP`, and push. If
  push is unavailable, keep the local commit and continue.
- A newly discovered human product/safety decision makes only the affected
  Phase `uncleared`; record it and continue independent work.

## Verification contract

- Static inventory proves whether each audited source is production-linked,
  fixture-only, retained infrastructure, or truly orphaned before deletion.
- Host fixtures cover candidate order/count, same-disk filtering, FAT type,
  media change, all configuration bounds, kernel-path traversal, boot0
  synthesis/validation, shorthand, native/overlay exclusivity, raw swap, and
  exact final parameter text.
- OVMF covers the config on the loaded FAT and on a separate same-disk FAT,
  zero and duplicate candidates, cross-disk exclusion, overlay boot,
  `rootpart=` native boot, missing kernel/config, and ignored LoadOptions.
- The ordinary amd64 IDE and xHCI USB-root images continue to reach `login:`.
- `make -j16` passes before q031 is closed.

## Completion definition

q031 is finished when p004, p002, and p003 are each `completed` or honestly
`uncleared`; P/W/M/Q contain the same actual evidence and residual findings;
and the master identifies the next dependency-ready installer Queue or its
remaining human source-template decision.

## Actual result

All three authorized Phases completed. `BOOTX64.EFI` now discovers the first
readable same-disk FAT16/FAT32 `/zedbsd.cfg`, loads its required configured
kernel, and emits the bounded common parameter record with the agreed
normalization and no LoadOptions or fixed-name fallback. The final independent
review aligned FAT media geometry with the kernel and added checked GOP and
framebuffer mapping bounds.

Evidence includes ordinary/sanitizer/analyzer host fixtures, BR-T43 handoff,
the seven-cell post-review q35/OVMF matrix, overlay and native UEFI boot,
cross-boot/PARTUUID cells, an amd64 SeaBIOS boot to `login:`, the retained
PC-98 M9 link, strict `BOOTX64.EFI` rebuild, and `make -j16`. The dead-source
audit removed only sources with proven no-owner or replacement evidence and
retained uncertain/test-owned code.

The later BIOS convergence goals are deliberately not smuggled into q031.
They are now defined as `ws013-p005` (independently accepted i386 PC/AT and
amd64 BIOS `/zedbsd.cfg`) and `ws013-p006` (PC-98 `/BOOTZBSD.CFG` using the
same grammar). During an extra, non-gating PC-98 default-root run, the kernel
entered successfully but the existing physical-disk registration remained
empty and `boot0` resolution failed; p006 must preflight and either repair or
separately prerequisite that baseline before claiming its loader acceptance.
