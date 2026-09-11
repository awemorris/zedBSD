# ws010-p004: three-platform boot acceptance

WSID: `ws010`

Phase ID: `p004`

Status: complete

Parent WS: [WS010](../ws.md)

## Objective

Prove that script migration preserves bootable disk images for amd64, PC-98
i386, and PC/AT i386.

## Work packages

- [x] Implement the required bounded Noct QEMU harness under WS010 tests.
- [x] Build and boot `build/amd64/hdd-image.img` with `qemu-system-x86_64`.
- [x] Build and boot `build/pcat/hdd-image.img` with `qemu-system-i386`.
- [x] Build and boot `build/pc98/hdd-image.img` with a `pc9821`-capable QEMU.
- [x] Record commands, emulator versions, output markers, and timeouts.

## Completion record

All three images reached `init: system running` and the `login:` prompt within
20 seconds. Emulator versions and profiles are recorded in
[`../tests/boot-results.md`](../tests/boot-results.md). The acceptance harness
is newly authored in this WS and does not use `.internal/`.

## Completion conditions

- Each disk image is built entirely through the Noct-scripted production path.
- Each emulator reaches the declared init-ready or login marker within a bounded timeout.
- A missing emulator, crash, hang, or structural-only image check is not counted as boot success.
- Results and any remaining limitations are recorded here and in WS010/master.
