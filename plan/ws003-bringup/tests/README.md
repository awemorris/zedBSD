# WS003 shared test cases

Parent: [WS003](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| BR-T00 | Physical inventory | Exact BIOS/UEFI/Secure Boot state and PCI/USB IDs are captured without modifying devices |
| BR-T10 | Host image | USB image generation is reproducible and partitions/files verify before writing media |
| BR-T20 | QEMU UEFI/BIOS | Firmware discovers USB and transfers control to the intended loader/kernel |
| BR-T21 | QEMU xHCI | Kernel enumerates xHCI and USB mass storage, selects the intended root despite added disks |
| BR-T22 | QEMU root I/O | Sustained read/write, sync, reboot, missing-root, and timeout behavior pass on disposable images |
| BR-T30 | Latitude cold boot | Repeated cold boots reach init/login/shell from USB with diagnostic logs |
| BR-T31 | Latitude root I/O | USB-root filesystem smoke and sustained I/O complete without reset/corruption |
| BR-T40 | CDC selection | Selected ACM or ECM/NCM profile interoperates and reconnects; device-role capability is proven first |
| BR-T50 | Physical network | Static or DHCP setup, peer reachability, and a bounded data transfer pass |

Each extracted Phase converts the applicable row into an executable script or
a hardware runbook and records its artifact path here.

