# WS003: real-hardware bring-up

Last updated: 2026-08-25

WSID: `ws003`

Status: planned; no Phase started

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: extract the exact Latitude hardware-inventory Phase, followed by
a separate QEMU USB-image/xHCI boot Phase.

Shared tests: [WS003 test index](tests/README.md)

## Phase registry

No Phase has started. The first extraction covers BR-00 only; BR-01–BR-04 may
then be split according to the xHCI audit from WS004.

## Goals

- Boot zedBSD from USB on the Dell Latitude 5320.
- Reach a stable init/login shell while continuing to use the intended USB
  mass-storage root.
- Establish usable diagnostics and at least one physical network path.

## WS completion conditions

WS003 is complete when USB boot reaches tier U5 in both the declared QEMU matrix
and on the target laptop, repeated cold boots reach a usable shell, the root
filesystem passes safe I/O tests, and one documented physical network path
passes configuration and transfer tests.

Target: Dell Latitude 5320, Intel 11th generation platform

## 1. Objective

Boot a reproducible zedBSD USB image on the target laptop, retain the USB mass
storage device as the root backing store, reach a stable login shell, establish
a diagnostic path, and make at least one physical network interface usable.

The target name alone is insufficient to select drivers. The exact machine
configuration and PCI/USB IDs are part of the first deliverable.

## 2. Definition of “USB boot works”

USB bring-up is divided into observable tiers so firmware success is not
confused with operating-system support:

| Tier | Required behavior |
| --- | --- |
| U0 — Firmware load | BIOS/UEFI discovers the USB device and transfers control to the zedBSD loader |
| U1 — Kernel entry | The loader loads the intended kernel and passes valid boot parameters |
| U2 — Kernel enumeration | The kernel enumerates the active USB host controller and mass-storage device |
| U3 — Root continuity | The kernel selects and mounts the intended USB-backed root, independent of discovery order |
| U4 — Stable system | init/login/shell work and sustained reads/writes complete without reset or corruption |
| U5 — Recovery | Timeouts, missing media, and controller errors fail visibly without hanging silently |

M1 requires U0–U5 in QEMU. M2 requires U0–U5 on the Latitude 5320.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| BR-00 | Planned | Exact target inventory: BIOS, UEFI mode, Secure Boot state, CPU, GPU, xHCI, NVMe, WLAN, Ethernet/USB adapters, and IDs | Physical laptop | Inventory is stored with commands and output summary; driver family assumptions are revised |
| BR-01 | Planned | Reproducible USB image layout and safe write/verify procedure | Current build/image pipeline | A disposable image is generated twice consistently and its partitions/files are inspected |
| BR-02 | Planned | QEMU boot through `qemu-xhci` and `usb-storage` | BR-01, xHCI work in HW track | U0–U5 pass in the declared BIOS/UEFI matrix |
| BR-03 | Planned | Stable boot/root device selection rather than enumeration-order assumptions | Bootloader/kernel parameter review | Root selection survives reordered storage-device attachment and reports actionable failure |
| BR-04 | Planned | Kernel xHCI and USB-storage continuity sufficient for USB root | xHCI, block layer, USB storage | Repeated QEMU I/O and reset/error tests pass |
| BR-05 | Planned | Latitude firmware-to-kernel USB boot | BR-00–BR-04 | Cold boots reach kernel reliably with captured diagnostics |
| BR-06 | Planned | Latitude USB root through init/login/shell | BR-05 | Repeated cold boots reach a usable shell; filesystem smoke test passes |
| BR-07 | Proposed | USB CDC diagnostic and/or network function selected and implemented | CDC profile decision, USB device/gadget capability | The selected profile interoperates with a documented host OS and recovers from reconnect |
| BR-08 | Planned | At least one working physical network path | BR-00, BR-06, relevant NET/HW item | DHCP or static configuration, ping, and data transfer pass on hardware |

## 4. QEMU USB matrix

The first Phase extracted from BR-01–BR-04 must state the precise matrix. The
intended minimum is:

- UEFI and the currently supported legacy/BIOS path, unless the loader supports
  only one and the limitation is explicitly recorded;
- an xHCI controller with USB mass storage, because an 11th-generation laptop
  cannot be assumed to expose UHCI/EHCI to the operating system;
- the USB device as the only boot disk, followed by a case with an additional
  disk to expose device-order assumptions;
- root mount, sustained file I/O, sync, and clean reboot;
- missing or delayed root device and an injected/observable timeout path.

An EHCI case may remain as regression coverage, but it is not a substitute for
the xHCI path.

## 5. USB CDC decision gate

“USB CDC” must be refined before coding:

- CDC ACM is appropriate for a serial diagnostic console/log channel.
- CDC ECM or NCM is appropriate for Ethernet over USB.
- A host-capable xHCI controller does not automatically make the laptop a USB
  device; the physical port/controller must support the required device/gadget
  role for zedBSD to expose a CDC function.

BR-00 therefore records USB controller roles. If the Latitude cannot expose a
device function, CDC device-mode work remains a separately testable target and
hardware diagnostics use an available serial, framebuffer, persistent-log, or
network path. The plan must not claim the laptop can provide CDC without this
evidence.

## 6. Safety and handoff

- Use a dedicated, disposable USB device for image writes.
- Resolve the exact block-device path and verify its size/identity before every
  destructive host operation.
- Do not write the internal NVMe device during initial USB bring-up.
- Preserve framebuffer/console diagnostics until an independent diagnostic
  channel is proven.
- Record partial success at the highest U-tier reached, along with the earliest
  failing transition and its logs.
