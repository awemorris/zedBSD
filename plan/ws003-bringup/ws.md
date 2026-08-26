# WS003: real-hardware bring-up

Last updated: 2026-08-26

WSID: `ws003`

Status: active; `ws003-p003` Partial, `ws003-p004` ready for a future Queue

Parent: [master plan](../master.md)

Last verified Phase: `ws003-p002` corrected the high-RSDP rejection, passes
OVMF at 4, 8, and 16 GiB, preserves the BIOS path, and passes BR-T32 3/3 on
the Latitude

Resume point: q012 finished with `ws003-p003` uncleared. Both Latitude xHCI
1.2 controllers attach, but EP0 device enumeration fails before USB storage.
Place `ws003-p004` in a future Queue and begin with the exact control-TRB
fixture. Hardware inventory remains incomplete.

Shared tests: [WS003 test index](tests/README.md)

## Phase registry

| Combined ID | Work item | Status | Result |
| --- | --- | --- | --- |
| `ws003-p001` | [BR-00 hardware inventory](phase001-hardware-inventory/phase.md) | Carried forward | Active host is WSL2; target DMI/PCI/USB evidence is unavailable |
| `ws003-p002` | [BR-05 Latitude UEFI memory map](phase002-uefi-memory-map/phase.md) | Complete | Four physical markers proved U1 and `RSDP=0x64ffe014`; the corrected image reaches ACPI/IRQ/HAL on hardware 3/3, while BR-T24 4/8/16-GiB OVMF and legacy BIOS remain passing |
| `ws003-p003` | [Latitude xHCI capability/MMIO bring-up](phase003-latitude-xhci-capability-mmio/phase.md) | Partial (`q012` uncleared) | Both physical xHCI 1.2 controllers pass capability validation and attach; BR-T33 then fails during EP0 enumeration before mass storage |
| `ws003-p004` | [Latitude xHCI device enumeration](phase004-latitude-xhci-device-enumeration/phase.md) | Ready; not queued | Correct Control TRB TD boundaries, port-reset completion, EP0 context, and cancellation recovery, then reach `usb-storage: sda` on hardware |

`ws003-p003` was the sole authorized item in q012. Its physical result closes
the PCI/BAR/capability boundary and extracts the first device-enumeration stop
to `ws003-p004`; q012 does not authorize that new Phase.

## Current xHCI handoff decisions

- The former `capabilities (13)` compound `ENODEV` failure is cleared on both
  physical functions; both report HCIVERSION 1.2 and `reject=00000000:ok`.
- Enable PCI Memory Space before every BAR MMIO read, but keep bus mastering
  deferred until DMA/controller startup.
- Diagnose the original and reassigned BAR and raw capability registers before
  choosing between a local ordering fix and bounded high-address MMIO support.
- Keep U2 controller/storage enumeration separate from U3/U4 root and login
  acceptance. The current first failure is the EP0 control-transfer path,
  before the storage-class driver or VFS.

## Goals

- Boot zedBSD from USB on the Dell Latitude 5320.
- Reach a stable init/login shell while continuing to use the intended USB
  mass-storage root.
- Establish usable diagnostics and at least one physical network path.

## WS completion conditions

WS003 is complete when USB boot reaches tier U5 in both the declared QEMU matrix
and on the target laptop, the frozen integrated image reaches a usable shell on
five consecutive final-acceptance cold boots, the root filesystem passes safe
I/O tests, and one documented physical network path passes configuration and
transfer tests.

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
| BR-05 | Complete | Latitude firmware-to-kernel USB boot | BR-00–BR-04 | U1 passes: corrected high-RSDP path reaches ACPI/IRQ/HAL on three cold boots |
| BR-06 | Planned; xHCI U2 sub-gate extracted | Latitude USB root through init/login/shell | BR-05, `ws003-p003`, `ws003-p004` | One physical success provisionally confirms each new boundary while implementation continues; after U4 is frozen, BR-T30 requires five consecutive shell boots and filesystem smoke passes |
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
