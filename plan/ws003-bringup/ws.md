# WS003: real-hardware bring-up

Last updated: 2026-08-28

WSID: `ws003`

Status: active; `ws003-p004` through `ws003-p009` complete in q013; q014
`ws003-p010` and physical U3 complete through BR-T41; q015 completed `p011`
through `p015`; q023 completed `ws003-p016` and the Python-free static image
parameter closure with a fresh 31-cell four-platform matrix

Parent: [master plan](../master.md)

Last complete Phases: q015 `ws003-p011`--`p015`. BR-T46 passed 31/31
production-loader cells across i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64
UEFI in the post-review `q015-br-t46-final-007` run. Earlier BR-T41 resolved
the intended UUID to `/dev/sda1`, mounted the
read-write data loop and root overlay, started init, and reached a root shell,
proving physical tier U3.

Resume point: return to the remaining physical U4/U5 work after q023. Physical
follow-up still includes BR-T31
sustained root I/O and, after U4 is otherwise frozen, BR-T30 five-boot
repeatability. Do not request an additional intermediate hardware boot now.
Hardware inventory remains incomplete.

Shared tests: [WS003 test index](tests/README.md)

## Phase registry

| Combined ID | Work item | Status | Result |
| --- | --- | --- | --- |
| `ws003-p001` | [BR-00 hardware inventory](phase001-hardware-inventory/phase.md) | Carried forward; WLAN ID captured | RTL8822CE is `10ec:c822`, subsystem `10ec:c130`; remaining target DMI/PCI/USB inventory is incomplete |
| `ws003-p002` | [BR-05 Latitude UEFI memory map](phase002-uefi-memory-map/phase.md) | Complete | Four physical markers proved U1 and `RSDP=0x64ffe014`; the corrected image reaches ACPI/IRQ/HAL on hardware 3/3, while BR-T24 4/8/16-GiB OVMF and legacy BIOS remain passing |
| `ws003-p003` | [Latitude xHCI capability/MMIO bring-up](phase003-latitude-xhci-capability-mmio/phase.md) | Partial (`q012` uncleared) | Both physical xHCI 1.2 controllers pass capability validation and attach; BR-T33 then fails during EP0 enumeration before mass storage |
| `ws003-p004` | [Latitude xHCI device enumeration](phase004-latitude-xhci-device-enumeration/phase.md) | Complete (`q013`) | BR-T34 reached `usb-storage: sda`; Control/EP0/reset and U2 are physically accepted |
| `ws003-p005` | [xHCI command and cancellation lifecycle](phase005-xhci-command-cancel-lifecycle/phase.md) | Complete (`q013`) | Fault fixtures and BR-T34 show safe command/cancel/DMA/slot ownership |
| `ws003-p006` | [xHCI halted-endpoint recovery](phase006-xhci-halted-endpoint-recovery/phase.md) | Complete (`q013`) | BR-T35 and physical BOT I/O clear EP0/bulk recovery and Normal-IN TD behavior |
| `ws003-p007` | [Shared DMA allocation synchronization](phase007-shared-dma-allocation-synchronization/phase.md) | Complete (`q013`) | BR-T36 and both physical controllers complete without allocation-registry corruption |
| `ws003-p008` | [xHCI device association lifetime](phase008-xhci-device-association-lifetime/phase.md) | Complete (`q013`) | BR-T37/hotplug and multi-device physical configuration retain the correct object association |
| `ws003-p009` | [xHCI SuperSpeed endpoint context](phase009-superspeed-endpoint-context/phase.md) | Complete (`q013`) | BR-T38 and the physical SuperSpeed storage configuration clear Slot/Endpoint Context |
| `ws003-p010` | [USB-storage flush capability](phase010-usb-storage-flush-capability/phase.md) | Complete (`q014`) | BR-T41 mounted the USB-backed writable overlay and reached init/login/root shell; the opcode-35 failure did not recur |
| `ws003-p011` | [common boot-parameter core and init selection](phase011-boot-parameter-core/phase.md) | Completed (`q015`, 2026-08-27) | BR-T42 passes; the bounded common parser and architecture-independent `init=` semantics are implemented |
| `ws003-p012` | [x86 boot-parameter handoff](phase012-x86-parameter-handoff/phase.md) | Completed (`q015`, 2026-08-27) | BR-T43 and all four production-loader runtime paths publish the same kernel-owned parameter string |
| `ws003-p013` | [boot slots and root-source selection](phase013-root-source-selection/phase.md) | Completed (`q015`, 2026-08-27) | BR-T44 and BR-T46 pass native/overlay selection on all four platforms plus UUID/PARTUUID discovery-order regressions on both amd64 firmware paths |
| `ws003-p014` | [multi-source swap activation](phase014-multi-swap/phase.md) | Completed (`q015`, 2026-08-27) | BR-T45 and every BR-T46 file/raw/mixed swap cell pass actual page-out, page-in, and content restoration |
| `ws003-p015` | [four-platform boot-parameter acceptance](phase015-x86-parameter-acceptance/phase.md) | Completed (`q015`, 2026-08-27) | BR-T46 passes 31/31 production-loader cells: PC/AT 7, PC-98 6, amd64 BIOS 9, and amd64 UEFI 9 |
| `ws003-p016` | [static image boot parameters and Python-regression removal](phase016-boot-parameter-header-dependency/phase.md) | Completed (`q023`, 2026-08-28) | BR-T47 and a fresh BR-T46 pass: one maintained definition feeds all x86 loaders and the kernel fallback; generated inputs, Python, and stale cross-build state are absent |

`ws003-p003` was the sole authorized item in q012. Its physical result closes
the PCI/BAR/capability boundary and extracts the first device-enumeration stop
to `ws003-p004`. q013 further separates the independently testable P1
command/cancel lifecycle into p005. q013 review added p006 endpoint recovery
and p007 shared-DMA synchronization. Continued review added p008 direct device
association and p009 SuperSpeed context; all six consumed the same passing
BR-T34 U2 observation. The independent U3 stop was isolated in p010 and is
cleared by BR-T41.

The public implemented parameter contract is
[documented separately](../../docs/reference/kernel-boot-parameters.md). It
uses four boot filesystem slots (`boot0`--`boot3`), mutually exclusive native
`rootpart` and explicit overlay modes, four ordered swap sources
(`swap0`--`swap3`), and architecture-independent `init`. The old `boot=` and
`root=` spellings and the provisional `loop0=`/`loop1=` names are not retained.

The boot-parameter implementation added a Python-generated header after WS010
had removed Python from the supported x86 image paths. It also exposed stale
cross-build state when `ZEDBSD_BOOT_PARAMETERS_FILE` changed. `ws003-p016`
removes that mechanism, makes the image default maintained source, and adapts
the affected regressions without reopening the p011--p015 public contract.

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
| BR-00 | In progress; WLAN ID captured | Exact target inventory: BIOS, UEFI mode, Secure Boot state, CPU, GPU, xHCI, NVMe, WLAN, Ethernet/USB adapters, and IDs | Physical laptop | RTL8822CE `10ec:c822`/subsystem `10ec:c130` is recorded; remaining inventory is stored with commands and output summary |
| BR-01 | Planned | Reproducible USB image layout and safe write/verify procedure | Current build/image pipeline | A disposable image is generated twice consistently and its partitions/files are inspected |
| BR-02 | Planned | QEMU boot through `qemu-xhci` and `usb-storage` | BR-01, xHCI work in HW track | U0–U5 pass in the declared BIOS/UEFI matrix |
| BR-03 | Complete (`q015`) | Stable boot/root device selection rather than enumeration-order assumptions | Bootloader/kernel parameter review | BR-T46 UUID and PARTUUID cells pass on both amd64 firmware paths with the auxiliary disk enumerated first; the PC/AT root/swap alias is rejected before publication |
| BR-04 | Planned | Kernel xHCI and USB-storage continuity sufficient for USB root | xHCI, block layer, USB storage | Repeated QEMU I/O and reset/error tests pass |
| BR-05 | Complete | Latitude firmware-to-kernel USB boot | BR-00–BR-04 | U1 passes: corrected high-RSDP path reaches ACPI/IRQ/HAL on three cold boots |
| BR-06 | In progress; U3 complete, one shell/X smoke boot passed | Latitude USB root through init/login/shell | BR-05, `ws003-p003`--`p010` | BR-T41 provisionally confirms U3 and a basic U4 path; BR-T31 sustained I/O and, after U4 is frozen, BR-T30 five consecutive shell boots remain |
| BR-07 | Planned after device identification | The user's Realtek USB LAN adapter works as a host-mode physical network path | Exact USB VID:PID/controller family, WS004 driver, WS005 integration | The adapter reconnects and passes DHCP/static and transfer tests on the Latitude |
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

## 5. USB LAN target decision

The selected first network target is one of the user's Realtek USB Ethernet
adapters connected to the Latitude's host-mode xHCI controller. This is not
CDC ACM, which is a serial communication profile. A standards-compliant CDC
ECM or CDC NCM interface can bind by interface class/subclass/protocol without
requiring a product-specific ID. That does not make all USB Ethernet devices a
single class: Realtek RTL8152/RTL8153 devices commonly expose vendor-specific
interfaces and need a Realtek-family backend plus a VID:PID/quirk table.

The implementation direction is therefore a common `usbnet` data/lifecycle
core, CDC ECM/NCM class frontends, and a separate Realtek-family frontend when
the target descriptors require it. Supporting several adapters means adding
their IDs to that one family driver, not creating one driver per product.
Descriptors decide which frontend is needed; marketing brand alone does not.

On FreeBSD, collect:

```sh
usbconfig list
usbconfig -d ugenBUS.ADDRESS dump_device_desc
usbconfig -d ugenBUS.ADDRESS dump_curr_config_desc
```

Record `idVendor`, `idProduct`, interface class/subclass/protocol, and the
FreeBSD attached driver. CDC device/gadget mode is no longer a dependency of
the Latitude network milestone and may be reconsidered separately later.

## 6. Secure Boot policy

NVMe and Secure Boot are independent. The initial Latitude policy is UEFI boot
with Secure Boot disabled; zedBSD image signing and key enrollment are deferred.
This does not prevent either USB or NVMe storage from being used as the UEFI
boot source. BR-00 still records the firmware setting for reproducibility.

References:

- UEFI Secure Boot authenticates UEFI images rather than selecting the storage
  protocol: <https://uefi.org/specs/UEFI/2.10/32_Secure_Boot_and_Driver_Signing.html>
- NetBSD's UEFI installation procedure explicitly uses Secure Boot disabled
  with either NVMe or other disks:
  <https://wiki.netbsd.org/Installation_on_UEFI_systems/>
- The Latitude 5320 firmware documents Secure Boot as a boot-configuration
  option and does not support legacy boot mode:
  <https://www.dell.com/support/manuals/en-us/latitude-13-5320-2-in-1-laptop/latitude_5320_sm/boot-configuration>

## 7. Safety and handoff

- Use a dedicated, disposable USB device for image writes.
- Resolve the exact block-device path and verify its size/identity before every
  destructive host operation.
- Do not write the internal NVMe device during initial USB bring-up.
- Preserve framebuffer/console diagnostics until an independent diagnostic
  channel is proven.
- Record partial success at the highest U-tier reached, along with the earliest
  failing transition and its logs.
