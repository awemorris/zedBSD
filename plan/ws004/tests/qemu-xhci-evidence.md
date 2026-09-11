# ws004-p004 QEMU xHCI evidence

Date: 2026-08-25

The production amd64 image was booted on QEMU q35 with a legacy IDE boot disk,
`qemu-xhci`, and a read-only `usb-storage` test disk. The legacy IDE device
keeps this Phase independent of the following USB-root Phase.

```sh
qemu-system-x86_64 -machine q35 -m 512 -smp 2 \
  -device piix3-ide,id=legacyide \
  -drive if=none,id=bootdisk,file=build/amd64/hdd-image.img,format=raw \
  -device ide-hd,bus=legacyide.0,drive=bootdisk \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=usbdisk,file=build/data.img,format=raw,readonly=on \
  -device usb-storage,bus=xhci.0,drive=usbdisk,id=stick
```

Observed production markers:

- `xhci: PCI controller, version=100 ports=8 slots=64 irq=0 MSI-X`;
- `usb0: device 1 port 1 ... configured`;
- `usb-storage: sdb blocks=65536 block-size=512`;
- the QEMU xHCI trace completes a 4096-byte media transfer; and
- init reaches `system running` and `login:`.

Hotplug acceptance used the QEMU monitor to remove `stick` after login and add
a second read-only backend. zedBSD reported device 1 on port 1 disconnected,
then configured device 1 on port 2 and registered it again as `sdb`. No QEMU
monitor error, kernel fatal, or xHCI failure was present.

The focused i386/PC-AT xHCI kernel selection also links and passes its image
contract. QEMU SuperSpeed media and injected controller faults remain useful
future coverage; the production root-port model and focused fixture retain the
USB3 speed distinction, and cancellation retains DMA memory if Stop Endpoint
cannot prove quiescence.
