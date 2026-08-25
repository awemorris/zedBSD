# WS004: hardware expansion

Last updated: 2026-08-25

WSID: `ws004`

Status: in progress; software foundation audit complete

Parent: [master plan](../master.md)

Last verified Phase: `ws004-p001` complete (hardware observations deferred)

Resume point: capture `ws003-p001`, then define the PCIe ECAM/MSI prerequisites
for HW-01 before implementing xHCI.

Shared tests: [WS004 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws004-p001`](phase001-foundation-audit/phase.md) | Complete | Software audit and two common fixes pass; Latitude evidence remains in `ws003-p001` |

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
| HW-01 | Planned | xHCI host-controller support sufficient for storage and HID | HW-00, existing USB core | QEMU xHCI enumeration, transfer, error, and reconnect tests; then Latitude logs |
| HW-02 | Planned | USB mass-storage behavior needed for root continuity | HW-01, block layer | QEMU USB-root U0–U5 and hardware USB-root tests |
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
