# WS004 shared test cases

Parent: [WS004](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| HW-T00 | PCIe/DMA/interrupts | BARs, capability walking, DMA widths/order, MSI/MSI-X setup/teardown, and timeout cleanup pass focused tests |
| HW-T10 | xHCI model | QEMU enumeration, control/bulk/interrupt transfers, reconnect, timeout, and controller reset pass |
| HW-T11 | USB storage | Root-continuity cases from WS003 pass through xHCI |
| HW-T20 | NVMe QEMU | Identify, namespace bounds, read/write/flush, concurrency, reset, and failure tests pass on disposable images |
| HW-T21 | NVMe hardware | Read-only identify precedes explicitly safe I/O; device IDs and stress/error logs are stored |
| HW-T30 | WLAN logic | Scan/association/key/error state tests pass against a bounded fixture without claiming radio hardware success |
| HW-T31 | WLAN hardware | Firmware load, scan, authentication, association, reconnect, and data-plane tests pass on the exact device |
| HW-T40 | i915 foundations | Device-independent UAPI/model tests pass; modeset/scanout/reset require target-hardware evidence |

QEMU/model and physical-hardware results are always separate evidence fields.

