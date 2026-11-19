zedBSD
======

zedBSD: https://github.com/awemorris/zedBSD

## How to run on Windows

Double click the `boot.bat`.

## How to run on Linux

Run this in the `zedbsd-nightly` directory, with a QEMU and virglrenderer
that support Venus (Vulkan) installed on the host:

```
qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -cpu host \
  -smp 4 \
  -m 8G \
  -drive if=pflash,format=raw,readonly=on,file=data/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=data/ovmf-vars.fd \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=boot,file=data/hdd-image.img,format=raw \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -device virtio-vga-gl,blob=on,hostmem=256M,venus=on \
  -display sdl,gl=on \
  -device usb-kbd,bus=xhci.0,port=3 \
  -device usb-tablet,bus=xhci.0,port=4 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device usb-net,bus=xhci.0,port=2,netdev=net0,msos-desc=on
```

## Other Software

This package bundles a Windows build of QEMU with Vulkan (Venus) support and
its runtime libraries. It is based on WINQ-EMU:

  QEMU with Vulkan support for Windows: https://github.com/cmspam/winq-emu-qemu
  virglrenderer for Windows:            https://github.com/cmspam/winq-emu-virglrenderer

The bundled QEMU and virglrenderer are built from these forks:

  https://github.com/awemorris/qemu-win32-vulkan
  https://github.com/awemorris/virglrenderer

## Licenses

Each bundled component keeps its own license. THIRD-PARTY.txt lists every
component with its version, license and where to get its source code, and
the LICENSES directory holds the full license texts.
