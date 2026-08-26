# WS003: real-hardware bring-up

Last updated: 2026-08-27

WSID: `ws003`

Status: active; `ws003-p004` through `ws003-p009` complete in q013; q014
`ws003-p010` and physical U3 complete through BR-T41

Parent: [master plan](../master.md)

Last complete Phases: q013 `ws003-p004`--`p009` and q014 `ws003-p010`.
BR-T41 resolved the intended UUID to `/dev/sda1`, mounted the read-write data
loop and root overlay, started init, and reached a root shell, proving U3.

Resume point: q014 is finished. Extract and separately authorize a bounded
Phase for the remaining U4/U5 work, including BR-T31 sustained root I/O and,
after U4 is otherwise frozen, BR-T30 five-boot repeatability. Do not request an
additional intermediate hardware boot now. Hardware inventory remains
incomplete.

Shared tests: [WS003 test index](tests/README.md)

## Phase registry

| Combined ID | Work item | Status | Result |
| --- | --- | --- | --- |
| `ws003-p001` | [BR-00 hardware inventory](phase001-hardware-inventory/phase.md) | Carried forward | Active host is WSL2; target DMI/PCI/USB evidence is unavailable |
| `ws003-p002` | [BR-05 Latitude UEFI memory map](phase002-uefi-memory-map/phase.md) | Complete | Four physical markers proved U1 and `RSDP=0x64ffe014`; the corrected image reaches ACPI/IRQ/HAL on hardware 3/3, while BR-T24 4/8/16-GiB OVMF and legacy BIOS remain passing |
| `ws003-p003` | [Latitude xHCI capability/MMIO bring-up](phase003-latitude-xhci-capability-mmio/phase.md) | Partial (`q012` uncleared) | Both physical xHCI 1.2 controllers pass capability validation and attach; BR-T33 then fails during EP0 enumeration before mass storage |
| `ws003-p004` | [Latitude xHCI device enumeration](phase004-latitude-xhci-device-enumeration/phase.md) | Complete (`q013`) | BR-T34 reached `usb-storage: sda`; Control/EP0/reset and U2 are physically accepted |
| `ws003-p005` | [xHCI command and cancellation lifecycle](phase005-xhci-command-cancel-lifecycle/phase.md) | Complete (`q013`) | Fault fixtures and BR-T34 show safe command/cancel/DMA/slot ownership |
| `ws003-p006` | [xHCI halted-endpoint recovery](phase006-xhci-halted-endpoint-recovery/phase.md) | Complete (`q013`) | BR-T35 and physical BOT I/O clear EP0/bulk recovery and Normal-IN TD behavior |
| `ws003-p007` | [Shared DMA allocation synchronization](phase007-shared-dma-allocation-synchronization/phase.md) | Complete (`q013`) | BR-T36 and both physical controllers complete without allocation-registry corruption |
| `ws003-p008` | [xHCI device association lifetime](phase008-xhci-device-association-lifetime/phase.md) | Complete (`q013`) | BR-T37/hotplug and multi-device physical configuration retain the correct object association |
| `ws003-p009` | [xHCI SuperSpeed endpoint context](phase009-superspeed-endpoint-context/phase.md) | Complete (`q013`) | BR-T38 and the physical SuperSpeed storage configuration clear Slot/Endpoint Context |
| `ws003-p010` | [USB-storage flush capability](phase010-usb-storage-flush-capability/phase.md) | Complete (`q014`) | BR-T41 mounted the USB-backed writable overlay and reached init/login/root shell; the opcode-35 failure did not recur |

`ws003-p003` was the sole authorized item in q012. Its physical result closes
the PCI/BAR/capability boundary and extracts the first device-enumeration stop
to `ws003-p004`. q013 further separates the independently testable P1
command/cancel lifecycle into p005. q013 review added p006 endpoint recovery
and p007 shared-DMA synchronization. Continued review added p008 direct device
association and p009 SuperSpeed context; all six consumed the same passing
BR-T34 U2 observation. The independent U3 stop was isolated in p010 and is
cleared by BR-T41.

## Current xHCI handoff decisions

- The former `capabilities (13)` compound `ENODEV` failure is cleared on both
  physical functions; both report HCIVERSION 1.2 and `reject=00000000:ok`.
- Enable PCI Memory Space before every BAR MMIO read, but keep bus mastering
  deferred until DMA/controller startup.
- Diagnose the original and reassigned BAR and raw capability registers before
  choosing between a local ordering fix and bounded high-address MMIO support.
- Keep U2 controller/storage enumeration separate from U3/U4 root and login
  acceptance. U2 and U3 are complete. BR-T41 also provides one init/login/root
  shell and X/`zterm` smoke result, but sustained I/O and recovery evidence are
  still required before full U4/U5 acceptance.

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
| BR-06 | In progress; U3 complete, one shell/X smoke boot passed | Latitude USB root through init/login/shell | BR-05, `ws003-p003`--`p010` | BR-T41 provisionally confirms U3 and a basic U4 path; BR-T31 sustained I/O and, after U4 is frozen, BR-T30 five consecutive shell boots remain |
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
