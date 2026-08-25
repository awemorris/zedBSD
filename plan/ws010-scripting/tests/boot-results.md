# WS010 boot acceptance results

Date: 2026-08-25

All images were rebuilt through their production `disk-image` targets after
the Noct migration. Each emulator was bounded and its VGA/GDC display was
captured at 20 seconds. The observed terminal marker was `login:`.

| Case | Emulator | Image | Result |
| --- | --- | --- | --- |
| SCT-T030 | QEMU 10.0.11, `qemu-system-x86_64 -machine pc -m 512 -smp 4` | `build/amd64/hdd-image.img` | PASS: init reports `system running`, then `login:` |
| SCT-T031 | QEMU 10.0.11, `qemu-system-i386 -machine pc -m 128` | `build/pcat/hdd-image.img` | PASS: init reports `system running`, then `login:` |
| SCT-T032 | qemu-pc98 11.0.93, `-M pc9821,pegc=off,coregraph=on -cpu 486 -smp 1 -m 64M` | `build/pc98/hdd-image.img` | PASS: init reports `system running`, then `login:` |

The PC-98 emulator was built in ignored `build/qemu-pc98`; upstream QEMU does
not supply the `pc9821` machine. This host lacked working passwordless sudo, so
its development packages were downloaded and extracted under ignored
`build/qemu-hostdeps` without changing the host installation.

No input, fixture, command, or implementation was obtained from `.internal/`.
