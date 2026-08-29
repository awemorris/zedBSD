# WS003 Phase 017: UEFI LoadOptions firmware compatibility

Last updated: 2026-08-29

WSID: `ws003`

Phase ID: `p017`

Combined ID: `ws003-p017`

Status: Implementation and QEMU verification complete; one physical acceptance
boot pending

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Trigger

The first post-q027 Latitude image stopped in `BOOTX64.EFI` immediately after
entry:

```text
A64 UEFI ENTRY
UEFI LoadOptions rejected: missing-nul
Validate LoadOptions: 0x8000000000000002
```

The prior QEMU acceptance patched the parameter record embedded in the EFI
image and did not supply nonempty `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions`.
Consequently it could not exercise this firmware boundary.

## Objective

Accept an intended bounded zedBSD parameter string from UEFI while ensuring
that legal opaque firmware `OptionalData`, and the known x86 whole
`EFI_LOAD_OPTION` firmware compatibility form, can never prevent the fallback
USB image from booting.

## Required behavior

- Treat `LoadOptionsSize` as authoritative; a final UTF-16 NUL is optional.
- Decode all UTF-16 and descriptor fields bytewise without an alignment
  assumption.
- Recognize text only when its first token uses a known zedBSD parameter name.
- Validate the complete packed `EFI_LOAD_OPTION` header, description, Device
  Path framing, and boundary before extracting `OptionalData`.
- Use the embedded image default for absent, empty, or unrecognized firmware
  data.
- Diagnose ignored non-text data, but do not turn it into an EFI boot failure.
- Keep invalid embedded image defaults fatal because they are a zedBSD build
  error rather than firmware input.

## Verification

`BR-T48` consists of:

1. the production BR-T43 host fixture covering terminated and
   length-delimited text, an unaligned pointer, a whole descriptor with empty
   or textual `OptionalData`, every truncated descriptor prefix, binary input,
   and both size boundaries;
2. ASan/UBSan and compiler static-analyzer runs of that fixture;
3. a test-only EFI entry wrapper that places a whole descriptor with empty
   `OptionalData` in the actual loaded-image protocol before invoking the
   renamed production entry, then boots it through q35/OVMF/xHCI/USB Storage;
4. the ordinary amd64 UEFI default cell; and
5. one physical boot of the rebuilt
   `build/amd64/hdd-image.img` on the Latitude.

Do not require repeated physical boots at this stage. Final five-boot
repeatability remains BR-T30 after implementation work is frozen.

## Completion conditions

- the Dell-style descriptor reaches init/login in the focused QEMU entry test;
- the ordinary amd64 UEFI default cell reaches login;
- the rebuilt production image passes `BOOTX64.EFI` structural validation and
  `make -j16`;
- the Latitude no longer stops at `Validate LoadOptions`; and
- the single physical run proceeds through kernel entry to the expected boot
  result, or exposes a later independent stop that is recorded separately.

## Evidence

- BR-T43 passes normally, with ASan/UBSan, and with compiler static analysis.
- `BOOTX64.EFI` builds and passes its structural checker.
- BR-T48 sets the real loaded-image `LoadOptions` to a complete descriptor,
  logs `UEFI LoadOptions descriptor: using OptionalData`, and reaches login
  through OVMF/q35/xHCI/USB Storage.
- The ordinary BR-T46 amd64 UEFI default cell reaches login at 4 GiB and on a
  512 MiB retry. One preceding 512 MiB run reached `/sbin/init` but timed out
  before login without a LoadOptions, kernel, xHCI, storage, or VFS error; this
  did not recur immediately and is not claimed as a p017 failure.
- Physical acceptance is pending the rebuilt image from this Phase.

## Reconsideration boundary

Stop and request the raw `LoadOptions` bytes only if the physical image still
stops in this conversion boundary. A later kernel, xHCI, storage, or root
failure is not a failure of this Phase and must be extracted separately.
