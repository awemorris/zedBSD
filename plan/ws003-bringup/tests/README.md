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
| BR-T25 | Host xHCI capability/MMIO fixture | Valid 1.0/1.1/1.2 capability layouts pass; zero/all-one and malformed extents fail distinctly; all-ones runtime MMIO is invalid; Memory Space enable precedes MMIO; 32/64-bit BAR assignment is transactional and fail-closed |
| BR-T26 | Host USB HCD teardown fixture | Failed checked quiesce retains the registered bus and blocks new URBs; a later successful quiesce/stop removes the same bus |
| BR-T27 | Host xHCI control/EP0/reset fixture | Setup, Data, and Status TRBs have exact xHCI 1.2 reserved-zero and TD boundaries; EP0 packet size/Average TRB Length and bounded port-reset outcomes are correct |
| BR-T28 | Host xHCI cancellation fixture | Running, Halted, Error, and Stopped endpoint recovery retains request/DMA ownership until Stop/Reset/Set-Dequeue or controller quiesce succeeds |
| BR-T30 | Final Latitude repeatability | One frozen integrated image reaches init/login/shell from USB on five consecutive cold boots; a failure breaks the sequence and is analyzed before retry |
| BR-T31 | Latitude root I/O | USB-root filesystem smoke and sustained I/O complete without reset/corruption |
| BR-T32 | Latitude UEFI entry | Three cold boots pass memory-map normalization and `ExitBootServices`, then show an unambiguous kernel-entry marker |
| BR-T33 | Latitude xHCI boundary confirmation | One boot identifies both xHCI functions and the boot-media controller attaches and enumerates USB mass storage without `capabilities (13)`, `boot-storage wait expired`, or `physical disks=0`; final repeatability belongs to BR-T30 |
| BR-T34 | Latitude xHCI device enumeration | One boot of the frozen p004 image reaches `usb-storage: sda` without an EP0 transfer failure or boot-storage timeout; later U3 errors are recorded separately |
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

BR-T25 directly shares the production xHCI capability arithmetic and PCI
enable-state implementation. It covers valid xHCI 1.0--1.2 snapshots,
reason-coded malformed layouts, scratchpad and relative xECP arithmetic,
32/64-bit BAR readback and transactional assignment, command-state rollback,
all-ones runtime-MMIO rejection, and fail-closed partial-write/readback-error
paths:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  drivers/pci.c \
  plan/ws003-bringup/tests/xhci-capability-mmio-test.c \
  -o /tmp/ws003-xhci-capability-mmio-test
/tmp/ws003-xhci-capability-mmio-test
```

The p003 teardown regression links the production USB core and verifies that a
failed checked HCD quiesce retains its registered bus, blocks new URBs, and can
be retried safely, while a later successful quiesce/stop removes that same bus:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  drivers/usb.c \
  plan/ws003-bringup/tests/usb-hcd-unregister-test.c \
  -o /tmp/ws003-usb-hcd-unregister-test
/tmp/ws003-usb-hcd-unregister-test
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
