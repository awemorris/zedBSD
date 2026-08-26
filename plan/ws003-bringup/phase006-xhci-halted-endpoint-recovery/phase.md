# WS003 Phase 006: xHCI halted-endpoint recovery

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p006`

Combined ID: `ws003-p006`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Recover an xHCI endpoint left Halted by a STALL before the next TD is
submitted. This applies to EP0 as well as bulk endpoints and keeps normal USB
Mass Storage recovery from turning a permitted GET_MAX_LUN STALL or later BOT
STALL into repeated timeouts.

## Confirmed defect

The USB Mass Storage driver sends `CLEAR_FEATURE(ENDPOINT_HALT)`, but that
device request does not change the host-controller Endpoint Context. xHCI must
also issue Reset Endpoint and then Set TR Dequeue to the software producer.
The same controller-side recovery is required when a control endpoint stalls.

## Scope

- Read the controller-owned Endpoint State before publishing a new TD.
- Treat Running as ready, Halted as Reset Endpoint followed by Set TR Dequeue,
  and Stopped/Error as Set TR Dequeue.
- Reject Disabled or unknown states without publishing a TD.
- Serialize recovery against request publication and checked device teardown.
- Treat every Normal IN Short Packet event as terminal, including one on a
  non-final chained TRB, and compute its cumulative actual length.
- Encode Normal TRB TD Size as bounded remaining packet count and require zero
  on the final TRB.
- Never repeat a command blindly after completion 19 without observing a
  state/action change.
- Cover the state decision in BR-T35 and share the final BR-T34 hardware boot
  with p004, p005, and p007--p009.

## Non-goals

- Changing the USB Mass Storage BOT protocol or adding multiple outstanding
  transfers.
- Treating a successful boot as exhaustive STALL fault injection.
- Requesting a separate physical boot for this Phase.

## Ordered work packages

- [x] Add a production-shared recovery-state decision fixture.
- [x] Recover Halted/Stopped/Error endpoints before TD publication.
- [x] Serialize endpoint recovery with submit and teardown.
- [x] Complete a split Normal IN TD at the Short event with cumulative actual.
- [x] Correct packet-based TD Size for 64-KiB-split Normal TDs.
- [x] Pass all focused host, build, legacy USB-root, BR-T24, and BR-T29 gates.
- [x] Feed the one shared BR-T34 result into this Phase.

## Completion conditions

- BR-T35 proves the exact recovery actions and fail-closed states.
- A recovery failure leaves the new TD unpublished and its temporary DMA
  released by software.
- The integrated automated USB-root gates pass.
- The single shared BR-T34 observation is recorded; no extra hardware run is
  requested.

## Actual results and evidence

The recovery path now reads controller-owned endpoint state before TD
publication, performs Reset Endpoint/Set TR Dequeue as required, rejects
disabled or unknown states, and serializes recovery with submit and teardown.
Normal IN Short Packet completion is terminal at any TRB in the TD and reports
the cumulative actual length; packet-based TD Size is bounded and zero on the
final TRB.

BR-T35 and the shared command/cancellation BR-T28 fixture passed. BR-T29 QEMU
remove/re-add, the focused xHCI/control/DMA/USB regressions, `make -j16`, and
`git diff --check` passed. The legacy-BIOS xHCI USB-root boot and BR-T24 OVMF
USB-root boots at 4, 8, and 16 GiB also passed.

BR-T34 then reached successful USB configuration, `usb-storage: sda`, and BOT
commands over the physical bulk endpoints without a halted-endpoint,
Set-Dequeue, short-packet, or recovery error. Together with BR-T35 this
completes the Phase. The later `05/20/00` response is a valid SCSI
CHECK CONDITION handed to `ws003-p010`, not an endpoint recovery failure.

## Interruption / resumption

No p006 work remains. Repeatability remains the later BR-T30 gate.

## Remaining debt and handoff

Full BOT STALL fault injection on real devices remains later U5 coverage. It
does not require an intermediate physical confirmation during q013.
