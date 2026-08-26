# WS003 shared test cases

Parent: [WS003](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| BR-T00 | Physical inventory | Exact BIOS/UEFI/Secure Boot state and PCI/USB IDs are captured without modifying devices |
| BR-T10 | Host image | USB image generation is reproducible and partitions/files verify before writing media |
| BR-T20 | QEMU UEFI/BIOS | Firmware discovers USB and transfers control to the intended loader/kernel |
| BR-T21 | QEMU xHCI | Kernel enumerates xHCI and USB mass storage, selects the intended root despite added disks |
| BR-T22 | QEMU root I/O | Sustained read/write, sync, reboot, missing-root, and timeout behavior pass on disposable images |
| BR-T23 | UEFI memory-map normalization | Valid sorted/unsorted maps canonicalize; malformed size, overflow, overlap, and capacity cases fail distinctly; OVMF still boots |
| BR-T24 | QEMU high-RSDP USB boot | The same production image boots OVMF/q35/xHCI at 4, 8, and 16 GiB; RSDP is above 1 GiB and ACPI/IRQ, four CPUs, USB root, and `login:` pass without fatal/storage errors |
| BR-T25 | Host xHCI capability/MMIO fixture | Valid 1.0/1.1/1.2 capability layouts pass; zero/all-one and malformed extents fail distinctly; Memory Space enable precedes MMIO and rollback is balanced |
| BR-T30 | Latitude cold boot | Repeated cold boots reach init/login/shell from USB with diagnostic logs |
| BR-T31 | Latitude root I/O | USB-root filesystem smoke and sustained I/O complete without reset/corruption |
| BR-T32 | Latitude UEFI entry | Three cold boots pass memory-map normalization and `ExitBootServices`, then show an unambiguous kernel-entry marker |
| BR-T33 | Latitude xHCI enumeration | Both xHCI functions are identified and the boot-media controller attaches and enumerates USB mass storage in three cold boots without `capabilities (13)` or `devices=0` |
| BR-T40 | CDC selection | Selected ACM or ECM/NCM profile interoperates and reconnects; device-role capability is proven first |
| BR-T50 | Physical network | Static or DHCP setup, peer reachability, and a bounded data transfer pass |

For the q011 diagnostic BR-T32 image, the top-right GOP marker is unary: one
large white block means boot services exited, two means the final map passed,
three means the bootstrap CR3 executed, and four means the kernel entry
instruction executed. A successful console initialization erases the panel.
With one large block, 1--7 small blocks underneath encode the final map error
enum. A final-map or `ExitBootServices()` error returned while boot services
remain live is printed as text instead.

The sparse ACPI physical-window allocator is exercised independently of
privileged page-table operations, including the Latitude RSDP address, mapping
reuse, a cross-page table, arithmetic overflow, exact exhaustion, and capacity
failure:

```sh
cc -std=c11 -I. -Iinclude -Wall -Wextra -Werror \
  src/hal/amd64/acpi-window.c \
  plan/ws003-bringup/tests/acpi-window-test.c \
  -o /tmp/ws003-acpi-window-test
/tmp/ws003-acpi-window-test
```

Each extracted Phase converts the applicable row into an executable script or
a hardware runbook and records its artifact path here.

BR-T23 is a focused host fixture:

```sh
cc -std=c11 -I. -Wall -Wextra -Werror \
  bootloader/uefi/memory-map.c \
  plan/ws003-bringup/tests/uefi-memory-map-test.c \
  -o /tmp/zedbsd-uefi-memory-map-test
/tmp/zedbsd-uefi-memory-map-test
```

BR-T24 is the production OVMF USB matrix. It defaults to the required 4, 8,
and 16-GiB cases and preserves per-capacity logs under the supplied output
directory:

```sh
plan/ws003-bringup/tests/uefi-high-memory-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-br-t24
```

Current physical/QEMU evidence is recorded in
[latitude-uefi-evidence.md](latitude-uefi-evidence.md).

The downstream physical xHCI boundary and required diagnostic facts are in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md).
