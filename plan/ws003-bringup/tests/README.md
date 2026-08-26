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
| BR-T29 | xHCI command/hotplug lifecycle | Command Completion events match the submitted TRB across IRQ/poll interleavings; QEMU auxiliary USB remove/re-add completes without stale commands, live DMA, or leaked slots |
| BR-T30 | Final Latitude repeatability | One frozen integrated image reaches init/login/shell from USB on five consecutive cold boots; a failure breaks the sequence and is analyzed before retry |
| BR-T31 | Latitude root I/O | USB-root filesystem smoke and sustained I/O complete without reset/corruption |
| BR-T32 | Latitude UEFI entry | Three cold boots pass memory-map normalization and `ExitBootServices`, then show an unambiguous kernel-entry marker |
| BR-T33 | Latitude xHCI boundary confirmation | One boot identifies both xHCI functions and the boot-media controller attaches and enumerates USB mass storage without `capabilities (13)`, `boot-storage wait expired`, or `physical disks=0`; final repeatability belongs to BR-T30 |
| BR-T34 | Latitude xHCI device enumeration | One boot of the frozen q013 p004--p009 image reaches `usb-storage: sda` without an EP0/command/recovery failure or boot-storage timeout; later U3 errors are recorded separately |
| BR-T35 | Host xHCI halted-endpoint recovery | Running submits directly; Halted uses Reset Endpoint then Set TR Dequeue; Stopped/Error use Set TR Dequeue; disabled/unknown states reject the TD |
| BR-T36 | Host shared-DMA lifecycle | Concurrent IRQ/SMP-style allocation, lookup/map, unlink/free, and destruction cannot corrupt or traverse a freed shared allocation-list node |
| BR-T37 | xHCI multi-device association lifetime | URB/endpoint operations resolve only their USB lifecycle-owned xHCI object; hotplug never traverses an unrelated freed controller-private device node |
| BR-T38 | xHCI SuperSpeed context | Slot Context preserves raw PORTSC Speed ID and SuperSpeed bulk Endpoint Context preserves companion `bMaxBurst` |
| BR-T39 | Host USB-storage flush capability | Bounded MODE/SENSE parsing plus BOT status/residue accounting select immutable sync-cache, write-through, FUA-from-first-write, or read-only policy before publication; unsafe state and runtime sync loss fail closed with the original error |
| BR-T40 | CDC selection | Selected ACM or ECM/NCM profile interoperates and reconnects; device-role capability is proven first |
| BR-T41 | Latitude USB-storage U3 confirmation | One boot of the frozen q014 image resolves `/dev/sda1`, mounts the read-write data loop and root overlay, and starts init without opcode-35 `05/20/00` or a false-success cache policy |
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

BR-T27, BR-T28/BR-T35, BR-T36, and BR-T38 share the production helper
contracts used by q013.  BR-T37's direct xHCI-device association lifetime is
covered by the extended HCD unregister fixture above and BR-T29 below.

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/xhci-control-ep0-reset-test.c \
  -o /tmp/ws003-xhci-control-ep0-reset-test
/tmp/ws003-xhci-control-ep0-reset-test

cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/xhci-cancel-command-test.c \
  -o /tmp/ws003-xhci-cancel-command-test
/tmp/ws003-xhci-cancel-command-test

cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror -pthread \
  drivers/dma.c plan/ws003-bringup/tests/dma-allocation-lock-test.c \
  -o /tmp/ws003-dma-allocation-lock-test
/tmp/ws003-dma-allocation-lock-test
```

BR-T29 boots from IDE while an auxiliary xHCI mass-storage device is removed
and re-added.  It proves that command, slot, request, device-association, and
DMA ownership survive the hotplug boundary without disrupting the live root:

```sh
plan/ws003-bringup/tests/xhci-hotplug-lifecycle.sh \
  build/amd64/hdd-image.img build/data.img /tmp/ws003-br-t29
```

The q013 legacy-BIOS USB-only gate always copies the supplied production image
before booting it.  It requires xHCI attach, device configuration,
`usb-storage: sda`, `/dev/sda1` root resolution, and `login:` and rejects the
bounded xHCI/storage failure signatures:

```sh
plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-legacy-xhci
```

q014 also runs that disposable-image gate with QEMU's cache disabled. This
requires the guest marker `cache=disabled ... flush=write-through`, enables
QEMU SCSI command logging, and rejects any redundant SYNCHRONIZE CACHE command:

```sh
USB_STORAGE_WRITE_CACHE=off USB_STORAGE_COMMANDLOG=on \
  plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-q014-write-through
```

BR-T24 is the production OVMF USB matrix. It defaults to the required 4, 8,
and 16-GiB cases and preserves per-capacity logs under the supplied output
directory.  Reusing an output directory explicitly truncates each run's guest
and QEMU logs before launch so stale markers cannot produce a false result:

```sh
plan/ws003-bringup/tests/uefi-high-memory-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-br-t24
```

BR-T39 is the q014 USB-storage durability-policy fixture. It shares the
production BOT, MODE SENSE, sense-classification, and policy helpers and covers
valid and malformed response lengths, CSW status/residue accounting,
WP/WCE/DPOFUA combinations, exact current-error `05/20/00` classification,
pre-publication policy selection, FUA on the first write, read-only unsafe
media, and sticky runtime sync failure with the original error:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/usb-storage-flush-test.c \
  -o /tmp/ws003-usb-storage-flush-test
/tmp/ws003-usb-storage-flush-test
```

BR-T41 was performed once after q014's host/build/QEMU gates passed and its
production image was frozen with a recorded path, size, and SHA-256. The
2026-08-27 Latitude result passes U3 and additionally reaches root login/shell
and X/`zterm`; it is recorded in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md). BR-T30 retains the later
five-consecutive-boot acceptance, and BR-T31 retains sustained root I/O.

Current physical/QEMU evidence is recorded in
[latitude-uefi-evidence.md](latitude-uefi-evidence.md).

The downstream physical xHCI boundary and required diagnostic facts are in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md).
