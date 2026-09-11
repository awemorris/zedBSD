# WS004 Phase 004: xHCI host controller

Last updated: 2026-08-25

Phase ID: `ws004-p004`

Status: complete QEMU storage milestone; physical/fault coverage retained

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Implement native xHCI host-controller support sufficient for the existing USB
core to enumerate and operate QEMU USB mass-storage and, subsequently, USB HID
devices. This Phase proves controller/port/command/transfer lifecycle; USB root
continuity remains the separate HW-02 Phase.

## Frozen scope

- PCI class `0x0c0330`, MMIO BAR ownership, bus mastering, and DMA constraints.
- xHCI 1.0/1.1 capability parsing, legacy ownership handoff, bounded reset,
  DCBAA/scratchpads, command ring, event ring/ERST, and interrupter 0.
- Root-port status/change/control for USB2 and USB3 protocol ranges.
- Enable Slot, Address Device, Configure Endpoint, and Disable Slot lifecycle.
- Control, bulk, and interrupt URBs through bounded TRB rings with explicit
  completion/error mapping and cancellation behavior.
- MSI/MSI-X through the completed PCI IRQ layer, with routed INTx fallback for
  modeled controllers.

The first implementation may serialize commands and one transfer per endpoint,
matching the current USB core, but it must return `EBUSY` rather than corrupt a
ring. Isochronous transfers and streams are outside this Phase.

## Work packages

- [x] Add the xHCI PCI driver and build/config registration.
- [x] Implement capability/extended-capability validation and ownership.
- [x] Implement DMA structures, reset/start/stop, command submission, and event
      draining with bounded waits.
- [x] Implement root-hub port status and reset semantics.
- [x] Implement slot/device/input/endpoint contexts and USB-core lifecycle glue.
- [x] Implement control/bulk/interrupt transfer TRBs and completion mapping.
- [x] Add focused structure/ring fixtures and configured builds.
- [x] Boot QEMU q35 with `qemu-xhci`, enumerate USB storage, exercise I/O and
      reconnect, and retain logs without claiming Latitude completion.

## Completion conditions

- A QEMU xHCI controller starts through the production PCI/MSI path.
- USB2 and USB3 root ports report connect/change/reset correctly.
- A QEMU USB mass-storage device enumerates and performs bounded read/write or
  read-only media tests through xHCI; disconnect/reconnect does not panic.
- Timeout, halted TRB, short packet, cancellation, and controller teardown have
  deterministic results in focused tests or QEMU fault cases.
- `make -j16` and relevant x86 kernel/image builds pass.

Physical Latitude observations remain a later acceptance layer. If QEMU exposes
a controller behavior that forces a USB-core contract change, stop and record
the concrete mismatch before widening the common API.

## Contract finding and decision gate

The pre-implementation audit found that the current HCD contract cannot model
xHCI device lifecycle cleanly:

- USB core always submits a wire `SET_ADDRESS` request. xHCI instead allocates
  a slot and performs `Address Device`; the USB request must not be sent.
- `drv_usb_hcd_ops.endpoint_enable/endpoint_disable` exist, but USB core never
  calls them after configuration or during teardown, so xHCI cannot issue
  `Configure Endpoint`/drop-context commands at the intended boundary.
- Root-hub scanning only adds devices. A disconnect does not detach interfaces,
  release the USB address, or give an HCD a point to disable its slot, so the
  declared reconnect acceptance cannot currently be implemented.

Two implementation policies are possible:

1. Recommended: extend the internal HCD contract with optional
   `device_enable`, `device_set_address`, and `device_disable` callbacks; use
   the existing endpoint callbacks from the core; and add generic disconnect
   teardown. UHCI/EHCI keep `device_set_address == NULL`, which means the core
   sends the ordinary USB request. xHCI handles it as a controller command.
2. Keep the API unchanged and make xHCI recognize and consume `SET_ADDRESS`
   URBs internally while lazily creating slots from the first endpoint-zero
   transfer. This hides core state transitions in request parsing and still
   requires ad-hoc teardown wiring.

Implementation is paused because choosing an internal lifecycle boundary
affects every present and future HCD, including USB HID hotplug work.

## Human decision

The explicit lifecycle design was approved on 2026-08-25. Add optional
`device_enable`, `device_set_address`, and `device_disable` HCD operations,
invoke the existing endpoint operations from the USB core, and implement
generic disconnect teardown. A null `device_set_address` retains the ordinary
wire request for UHCI/EHCI; xHCI implements the controller command instead.

## Result

Completed as a QEMU storage software milestone on 2026-08-25.

- The USB core now has the approved optional device lifecycle, invokes endpoint
  lifecycle callbacks, and tears down disconnected devices and addresses.
- The native xHCI 1.0/1.1 driver validates register ranges, handles ownership,
  scratchpads, DCBAA/command/event rings, slot and endpoint contexts, control,
  boundary-split bulk, and interrupt transfers. Port-change work is deferred
  from IRQ context to a retained-notification kernel worker.
- QEMU q35 uses the production PCI/ECAM/MSI-X path, enumerates USB mass storage,
  completes a 4096-byte read-only media transfer, reaches `login:`, disconnects
  the device, and reconnects a replacement without panic. Evidence is in
  [qemu-xhci-evidence.md](../tests/qemu-xhci-evidence.md).
- The xHCI model fixture and amd64/i386 configured kernel builds pass, followed
  by the repository `make -j16` build. No repository-wide `make check` target
  was used.

The first SuperSpeed device on QEMU, injected halted/error TRBs, and physical
Latitude behavior remain additional coverage rather than claims of this
software milestone. Cancellation issues Stop Endpoint and Set TR Dequeue; if
Stop Endpoint fails, DMA memory is intentionally retained instead of risking a
late device write. USB-root continuity is deliberately left to the next Phase.
