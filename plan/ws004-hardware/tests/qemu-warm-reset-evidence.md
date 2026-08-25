# ws004-p007 QEMU warm-reset evidence

Date: 2026-08-25

QEMU: `qemu-system-x86_64` 10.0.11 (Debian package
`1:10.0.11+ds-0+deb13u1`)

## Reproduction and boundary

The production BIOS image reproduced the second-kernel failure with both
`-smp 1` and `-smp 2`:

```text
12S2 MB P1 VF E6 LD GO
boot: kernel heap, process, and scheduler initialization
fatal: src/hal/amd64/lib.c:76: hal_set_allocator must be called exactly once
```

The loader-side program header remained correct, but memory inspection showed
that `.bss` from physical `0x289000` retained the first kernel. Temporary
loader verification stopped before kernel entry, proving that VFS shutdown and
AP startup were not the first failing boundary.

## Corrected IDE run

The production control command was:

```sh
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file=/tmp/q008-p007-final-ide.img,format=raw,if=ide
```

After initial login, `/sbin/reboot` was requested three consecutive times.
The debug log contained four `12S2 ... LD GO` loader sequences and four
`login:` prompts, with no allocator fatal, disk error, or stale-state marker.

## Corrected USB integration run

The combined USB command was:

```sh
qemu-system-x86_64 -machine q35 -m 512 -smp 2 \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=usbboot,file=/tmp/q008-p007-final-usb.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1
```

The first and second boots each registered writable `sda`, mounted the two loop
images, and reached `login:`. No `loop1`, USB-storage, or xHCI error occurred.
The disposable image and complete debug logs remained under `/tmp`; no
repository-wide `make check` target was used.
