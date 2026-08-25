# WS004 shared test cases

Parent: [WS004](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| HW-T00 | PCIe/DMA/interrupts | BARs, capability walking, DMA widths/order, MSI/MSI-X setup/teardown, and timeout cleanup pass focused tests |
| HW-T01 | ECAM/MSI HAL contract | Canonical source parsing, MCFG validation, vector allocation/exhaustion/reuse, PCI register images, rollback, in-flight unregister, and real QEMU delivery pass |
| HW-T10 | xHCI model | QEMU enumeration, control/bulk/interrupt transfers, reconnect, timeout, and controller reset pass |
| HW-T11 | USB storage | Root-continuity cases from WS003 pass through xHCI |
| HW-T12 | USB overlay writes | USB-backed `DATA.IMG` login/write/fsync/readback/cold-persistence passes without loop/FAT/BOT/xHCI errors; IDE is the control |
| HW-T13 | PC/AT warm reset | Three consecutive guest reboots reach fresh login with clean kernel BSS state |
| HW-T20 | NVMe QEMU | Identify, namespace bounds, read/write/flush, concurrency, reset, and failure tests pass on disposable images |
| HW-T21 | NVMe hardware | Read-only identify precedes explicitly safe I/O; device IDs and stress/error logs are stored |
| HW-T30 | WLAN logic | Scan/association/key/error state tests pass against a bounded fixture without claiming radio hardware success |
| HW-T31 | WLAN hardware | Firmware load, scan, authentication, association, reconnect, and data-plane tests pass on the exact device |
| HW-T40 | i915 foundations | Device-independent UAPI/model tests pass; modeset/scanout/reset require target-hardware evidence |

QEMU/model and physical-hardware results are always separate evidence fields.

## HW-00 host regressions

The foundation-audit regressions are ordinary host binaries and do not use a
repository-wide test target:

```sh
cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  drivers/dma.c plan/ws004-hardware/tests/dma-constraints-test.c \
  -o /tmp/ws004-dma-test
/tmp/ws004-dma-test

cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  drivers/pci.c plan/ws004-hardware/tests/pci-rescan-test.c \
  -o /tmp/ws004-pci-test
/tmp/ws004-pci-test

cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  drivers/pci.c plan/ws004-hardware/tests/pcie-capability-test.c \
  -o /tmp/ws004-pcie-test
/tmp/ws004-pcie-test
```

## HW-T01 evidence

`ws004-p003` provides the following focused evidence:

- exact acceptance and rejection cases for `PCI SSSS:BB:DD.F`;
- valid, truncated, overlapping, overflowing, and bad-checksum MCFG fixtures;
- reserved-vector exclusion, exhaustion, reuse, and teardown under in-flight
  dispatch;
- conventional MSI and MSI-X register images, width rejection, masking order,
  and complete rollback; and
- real interrupt delivery and detach on a deterministic MSI-capable PCIe device
  under QEMU `q35`.

The source parser, MCFG parser, PCI rescan/capability, and PCI MSI fixtures are
compiled directly with `cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror` and
their matching source file. The QEMU fixture is linked only when
`CONFIG_KERNEL_TEST_CHECKPOINTS=y`; it boots `q35` with `-device edu` and checks
allocator exhaustion/reuse, a delivered MSI, and continued progress through
`login:`. It does not use `make check` or material from `.internal/`.

## HW-T10 xHCI evidence

The focused arithmetic fixture covers scratchpad decoding, 64-KiB-safe Normal
TRB splitting, USB2/USB3 speed flags, interrupt interval encoding, short-packet
lengths, transfer-event ownership, Link TRB chain/cycle wrap at 254→0, and the
cancellation DMA-retention rule:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  plan/ws004-hardware/tests/xhci-model-test.c \
  -o /tmp/ws004-xhci-model-test
/tmp/ws004-xhci-model-test
```

The USB-storage SCSI fixture covers fixed and descriptor sense decoding plus
the MODE SENSE(6) write-protect bit used by HW-T12 failure injection:

```sh
cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  plan/ws004-hardware/tests/usb-storage-scsi-test.c \
  -o /tmp/ws004-usb-storage-scsi-test
/tmp/ws004-usb-storage-scsi-test
```

The i386 build selection is `tests/config-pcat-xhci.mk`. Runtime acceptance and
the exact QEMU command are recorded in
[qemu-xhci-evidence.md](qemu-xhci-evidence.md).

## HW-T11 USB boot/root continuity

`ws004-p005` owns this matrix. It must use production bootloader and kernel
paths, not a test-only disk pointer:

- legacy BIOS and supported x64 UEFI boot with the system image attached only
  through q35 `qemu-xhci` USB mass storage;
- the same cases with a non-boot IDE disk and another USB disk attached in both
  orders;
- exact selection by the approved MBR/GPT identity, with zero and duplicate
  matches rejected rather than resolved by discovery order;
- delayed device discovery inside the declared bound and missing media beyond
  it, both with visible diagnostics and no infinite wait;
- login followed by sustained reads and a bounded write/sync/readback on a
  disposable copy of the image; and
- clean reboot plus at least one disconnect/reset error observation without
  claiming physical Latitude completion.

Every evidence record includes the QEMU version, complete command line, image
identity values, highest WS003 U-tier reached, and first failing transition.
The current BIOS/UEFI, ordering, delayed/missing-media, I/O, and reboot findings
are recorded in [qemu-usb-root-evidence.md](qemu-usb-root-evidence.md). Identity
and bounded discovery pass. HW-T12 write/read-only-injection evidence is
recorded by `ws004-p006`; HW-T13 diagnosis and repeated reboot evidence is in
[qemu-warm-reset-evidence.md](qemu-warm-reset-evidence.md).
