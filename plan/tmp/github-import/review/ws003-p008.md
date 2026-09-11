# WS003 Phase 008: xHCI device association lifetime

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p008`

Combined ID: `ws003-p008`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Remove lockless traversal of controller-private xHCI device nodes from URB,
endpoint, and teardown paths so one device can disconnect while another
device performs I/O without list-node use-after-free.

## Confirmed defect

`find_device()` traversed `c->devices` without synchronization while enable
prepended and release unlinked/freed nodes. An URB keeps its own generic USB
device alive, but did not protect unrelated xHCI nodes traversed before the
matching node was found.

## Scope and fixed design

- Store the controller-private association directly in the generic USB device.
- Let the USB device lifecycle retain the associated xHCI object; the raw HCD
  value is not an independent reference.
- Store the xHCI device directly in each accepted request.
- Publish/clear the association and mutate the diagnostic device list under
  `active_lock`; do not traverse that list on the URB fast path.
- Share the final BR-T34 hardware observation with every q013 Phase.

## Ordered work packages

- [x] Add generic per-device HCD-private association accessors.
- [x] Remove xHCI fast-path device-list traversal.
- [x] Serialize association publication/removal and list mutation.
- [x] Pass BR-T37, BR-T29 hotplug, focused USB lifecycle, build, and USB-root
      gates.
- [x] Feed the one shared BR-T34 result into this Phase.

## Completion conditions

- A request never traverses or retains an unrelated xHCI list node.
- Association removal occurs only after checked quiesce/Disable Slot and zero
  HCD-owned URBs.
- Automatic hotplug and USB-root gates pass, then the one BR-T34 observation
  is recorded.

## Actual results and evidence

- `drv_usb_device_hcd_data()` and `drv_usb_device_set_hcd_data()` provide the
  generic USB-lifecycle-owned controller association. xHCI publishes and
  clears its direct `xhci_device` value around checked device ownership, and
  accepted requests retain that exact device instead of rescanning
  `c->devices`.
- `active_lock` now covers association/list publication and removal; URB,
  endpoint, completion, cancellation, and teardown paths use the direct
  association or the request-owned value. The controller list remains only a
  diagnostic/teardown registry rather than an URB fast-path lookup.
- BR-T37 passed through the extended USB HCD lifecycle fixture and BR-T29.
  BR-T29 observed two configurations, one disconnect, and two storage
  registrations without a stale association or retained-resource diagnostic.
- `make -j16`, applicable host regressions, `git diff --check`, the legacy BIOS
  q35/xHCI USB-only root gate, and BR-T24 at 4, 8, and 16 GiB passed with
  candidate SHA-256
  `bd3aa801ac890deabb5f0ad4b6f3388e5137992e9f6f81e8d912af4abad7585f`.
- The one shared BR-T34 Latitude run configured multiple physical devices and
  associated the SuperSpeed boot medium with the correct storage object through
  `usb-storage: sda` and BOT I/O. No stale association, wrong-device lookup,
  disconnect, or retained-resource diagnostic occurred. This completes the
  Phase; the later SCSI flush-capability failure is owned by `ws003-p010`.

## Remaining debt and handoff

Generic USB topology readers (`foreach`/`find`/dump), the USB driver registry
and unregister/detach path, and a deferred reaper for objects retained after
`EBUSY` still need explicit synchronization/lifetime contracts. Those items
are outside the current Latitude boot path and are not claimed complete by
BR-T37; they remain later common-USB lifecycle work.
