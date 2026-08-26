# WS004: hardware expansion

Last updated: 2026-08-26

WSID: `ws004`

Status: active; q010 automatic USB/heap gate complete, manual acceptance pending

Parent: [master plan](../master.md)

Last verified Phase: `ws004-p008` and resumed `ws004-p006` complete their
automatic QEMU milestones; detailed manual USB acceptance is pending

Resume point: record the user's detailed manual USB acceptance separately, or
extract the next planned hardware Phase. The automatic 500-boot gate passed.

Shared tests: [WS004 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws004-p001`](phase001-foundation-audit/phase.md) | Complete | Software audit and two common fixes pass; Latitude evidence remains in `ws003-p001` |
| [`ws004-p002`](phase002-pcie-xhci-prerequisites/phase.md) | Complete software milestone | Config limits, extended capabilities, bridge-tree scan, and contract evidence pass |
| [`ws004-p003`](phase003-ecam-msi/phase.md) | Complete software milestone | Fixed HAL API, MCFG/ECAM, amd64 vector allocation, PCI MSI/MSI-X lifecycle, and real QEMU delivery pass; unavailable cross-target toolchains are recorded |
| [`ws004-p004`](phase004-xhci/phase.md) | Complete QEMU storage milestone | Native xHCI uses MSI-X, enumerates and reads USB storage, and passes disconnect/reconnect; SuperSpeed/fault injection and Latitude evidence remain follow-ups |
| [`ws004-p005`](phase005-usb-root/phase.md) | Partial; automatic runtime cleared | BIOS/UEFI identity and revised 500-boot writable-root gate pass; malformed-handoff fixture and detailed manual acceptance remain |
| [`ws004-p006`](phase006-usb-overlay-write/phase.md) | Complete automatic QEMU milestone | URB correction plus p008 heap fix pass focused tests and the revised 500-boot gate; manual acceptance is pending |
| [`ws004-p007`](phase007-warm-reset/phase.md) | Complete | Native-mode ELF64 BSS clearing fixes stale allocator state; three IDE reboots and a USB reboot reach login |
| [`ws004-p008`](phase008-smp-heap-integrity/phase.md) | Complete | Unified kernel heap lock domain, corrected aligned-prefix arithmetic, controls, and 500-boot combined gate pass |

### MSI follow-up register

| Item | Initial `ws004-p003` treatment | Resume condition |
| --- | --- | --- |
| Multi-message conventional MSI | Deferred; one registration and one message only | A consumer requires multiple power-of-two messages and has focused ordering tests |
| Dynamic MSI affinity | Deferred; mappings target CPU 0 | A public remap operation can return a replacement address/event pair safely |
| arm64 IORT/GIC ITS backend | Deferred; public signature preserved and port returns unsupported | An arm64 PCIe platform Phase supplies firmware and interrupt-controller fixtures |
| Non-PCI message source prefixes | Deferred; only canonical PCI BDF is accepted | A concrete platform device needs message interrupts and defines stable source identity |

Candidate order after p001 is HW-01/HW-02, HW-10/HW-11, HW-20/HW-21, and
HW-30; each candidate becomes a Phase only when its inputs and acceptance
environment are available.

## Goals

- Provide the reusable PCIe, DMA, interrupt, reset, and firmware foundations
  required by the target laptop.
- Implement xHCI/USB-root, NVMe, the selected WLAN device, and i915 foundations
  as native zedBSD drivers.
- Keep modeled/QEMU results separate from physical-hardware results.

## WS completion conditions

WS004 is complete when the common hardware facilities pass focused regression
tests and the selected xHCI, NVMe, WLAN, and i915 driver scopes pass their
declared lifecycle and recovery tests on the Latitude 5320. Unsupported devices
and firmware constraints must be explicitly documented.

Primary physical target: Dell Latitude 5320

## 1. Objective

Build the reusable kernel foundations and native drivers needed for the target
laptop, beginning with xHCI/USB-root support and NVMe, followed by the exact WLAN
controller and i915 graphics generation discovered by hardware inventory.

## 2. Shared foundations

The driver work must audit and, where necessary, harden these common facilities
before individual drivers duplicate them:

- PCI/PCIe enumeration, BAR mapping, capabilities, and power state;
- legacy interrupts, MSI, and MSI-X with teardown and error handling;
- DMA allocation/mapping, address-width constraints, ordering, and cache
  coherency;
- bounded waits, controller reset, cancellation, and device removal;
- ACPI/firmware data used by the target devices;
- firmware-blob loading policy, provenance, versioning, and failure messages;
- block and network queue integration under concurrency.

IOMMU support is evaluated from the target firmware and DMA model. If it is not
implemented initially, the security and addressability limitation is explicit.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| HW-00 | Complete (software scope) | PCIe/DMA/interrupt capability audit and common fixes | BR-00 hardware inventory deferred | Focused host tests and amd64 build pass; physical findings remain separate |
| HW-01 | Complete read-only QEMU milestone | xHCI host-controller support sufficient for storage and future HID | HW-00, existing USB core | QEMU xHCI enumeration, bounded media read, and reconnect pass; writable-root correctness is HW-02/p006 |
| HW-02 | Complete automatic QEMU milestone; manual acceptance pending | Stable USB identity/discovery, writable overlay, bounded read-only rejection, and warm reboot | HW-01, block layer, approved selector decision | Revised HW-T12 500-copy gate passes; record the user's detailed manual acceptance separately before physical USB-root claims |
| HW-10 | Planned | NVMe controller, admin/I/O queues, namespaces, and block integration | HW-00 | QEMU NVMe install/mount/I/O/reset tests pass |
| HW-11 | Planned | NVMe verification on the Latitude controller | HW-10, BR-00 | Identify/read-only first, then disposable-range I/O and stress without corruption |
| HW-20 | Proposed | WLAN architecture and exact-device driver | BR-00, HW-00, firmware policy | Scan, authenticate, associate, and exchange data on hardware |
| HW-21 | Proposed | Testable WLAN hardware abstraction or protocol fixture | HW-20 design | Driver state/error paths can be tested without claiming QEMU emulates the laptop radio |
| HW-30 | Proposed | i915 hardware foundations for the discovered 11th-generation GPU | BR-00, HW-00, GFX UAPI | Modeset/scanout and recovery on hardware; model tests for device-independent layers |

## 4. NVMe sequence

1. Specify controller ownership, queue memory, PRP handling, namespace mapping,
   flush semantics, timeouts, and reset behavior.
2. Implement identify and read-only namespace discovery.
3. Add block I/O, flush, concurrent queues, bounds checks, and error recovery.
4. Test using QEMU's NVMe device with disposable images, including power-cycle
   and reset simulations available in the harness.
5. On the laptop, begin with controller identification and read-only access.
   Destructive tests require an explicitly disposable namespace or device.

Native UFS root on NVMe is a later acceptance case after the driver is stable;
it is not needed to prove the initial USB-root milestone.

## 5. WLAN sequence

WLAN implementation cannot be selected from the laptop model name. BR-00 first
records the vendor/device/subsystem IDs and required firmware. The resulting
Phase defines:

- the boundary between hardware driver, 802.11 state/frames, and the `wpa`
  userspace backend;
- scan and association event delivery;
- key installation and sensitive-data handling;
- firmware loading, reset, radio-kill, power, and reconnect behavior;
- data-plane integration with the existing network stack.

QEMU does not provide a faithful substitute for the expected Intel laptop WLAN
or Tiger Lake i915 device. Host-side state-machine tests, a constrained test
double, or PCI passthrough may validate separable logic, but final completion
requires the exact hardware.

## 6. Driver completion rule

A driver is complete only when the common lifecycle is covered: discovery,
normal operation, concurrency, timeout, reset, detach/shutdown, and diagnostic
reporting. A probe-only implementation or a command that returns success while
using a stub is partial, not complete.
