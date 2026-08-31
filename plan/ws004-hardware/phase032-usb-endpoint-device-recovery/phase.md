# WS004 Phase 032: checked USB endpoint-halt and direct-root device recovery

Last updated: 2026-08-31

WSID: `ws004`

Phase ID: `p032`

Combined ID: `ws004-p032`

Work item: `HW-28`

Test case: `HW-T26`

Status: in progress (`q047`); p031 and p006 automatic dependencies complete

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Provide one checked USB-core recovery contract for endpoint STALL and a
conservative synchronous device reset on xHCI, UHCI, and EHCI.  A class driver
must be able to recover a halted bulk or interrupt endpoint without inspecting
HCD-private toggle or ring state, and a single effective function owner must be
able to reset a directly attached device after draining its URBs without
replacing the retained USB device, interface, or binding objects.

This Phase removes the reset/STALL prerequisite which currently blocks
`ws006-p008`.  It does not implement USB HID itself.

## Dependencies

- `ws004-p010`: retained USB configurations, alternates, endpoints, and
  interface ownership.
- `ws004-p011`: exact xHCI per-endpoint URB, completion, cancellation, callback,
  ring, and DMA ownership.
- `ws004-p015`: device binding transactions, interface I/O admission,
  endpoint-zero serialization, alternate/configuration transactions, and
  checked callback drain.
- `ws004-p016`: controller-proven UHCI/EHCI retirement and data-toggle
  continuity.
- `ws004-p031` / `HW-27` / `HW-T25`: concurrent legacy-HCD interrupt traffic,
  hotplug, and runtime ownership needed before the same recovery contract can
  be claimed for USB 1.1 HID.

Downstream: `ws006-p008` may enter a Queue only after this Phase and p031 have
completed their automatic milestones.

## Audited starting point

The current tree has the required ownership pieces but no common recovery
transaction:

- `include/drivers/usb.h` declares `drv_usb_device_reset()`, while
  `src/drivers/usb.c` returns `ENOTSUP` unconditionally.
- `struct drv_usb_endpoint` has no core-owned halted state.
  `drv_usb_hcd_complete()` publishes `DRV_USB_URB_STALL` directly to the
  callback, and a later `drv_usb_urb_submit()` is not rejected by the core.
- `struct drv_usb_hcd_ops` has endpoint enable/disable and root-port reset
  callbacks, but no callback which certifies that host-side endpoint state has
  been reset after the device accepts `CLEAR_FEATURE(ENDPOINT_HALT)`.
- xHCI already has checked Reset Endpoint and Set TR Dequeue machinery, but
  invokes it implicitly before every new TD.  That recovery is not ordered
  after a successful device-side clear-halt request.
- UHCI and EHCI retain the next data toggle in endpoint HCD-private storage.
  Their endpoint enable/disable callbacks currently do not distinguish a
  schedule-only re-enable during rollback from a confirmed device-side
  endpoint reset which requires DATA0.
- USB Mass Storage sends its own clear-halt request and directly overwrites
  endpoint HCD-private data.  On xHCI it additionally relies on the next
  enqueue to perform implicit ring recovery.  This is a class-specific breach
  of the general USB/HCD boundary.
- Root-port reset, xHCI checked device quiesce/Disable Slot, device enable, and
  address assignment already exist.  There is no USB hub class driver, and all
  currently enumerated non-root devices are direct children of the emulated
  root hub.

## Frozen public kernel-driver contract

No user-visible UAPI, ioctl, or device node is added.

Add the following class-driver operation to `include/drivers/usb.h`:

```c
int drv_usb_endpoint_clear_halt(struct drv_usb_endpoint *endpoint);
```

Implement the already declared operation:

```c
int drv_usb_device_reset(struct drv_usb_device *device);
```

Both are synchronous kernel-driver APIs.  Neither waits for a caller-owned URB
or invokes cancellation on the caller's behalf.  A class driver must invoke
them from a worker or another non-callback context after its relevant URBs have
completed or passed `drv_usb_urb_cancel()` and `drv_usb_urb_drain()`.

Calling either operation from the terminal callback of an URB which still
owns interface admission is not supported.  The operation returns `EBUSY`
rather than waiting on its own callback/HCD ownership.

`drv_usb_endpoint_hcd_data()` and `drv_usb_endpoint_set_hcd_data()` remain HCD
association helpers.  A class driver must not use them to repair protocol
state.  The USB Mass Storage uses in this tree must be removed.

## Core endpoint-halt state

Add a private atomic halted state to each nonzero endpoint.

- A bulk or interrupt URB completed as `DRV_USB_URB_STALL` latches the endpoint
  halted before the terminal status and callback are published.
- Endpoint zero is not latched: a control-request STALL must not prevent the
  next standard control request.  Isochronous endpoints do not use endpoint
  halt and are also excluded.
- Submission to a latched endpoint fails with `EPIPE` before HCD enqueue and
  transfers no HCD, ring, descriptor, or DMA ownership.
- A terminal callback which tries to rearm immediately therefore sees
  `EPIPE`.  Recovery is deferred until that callback has returned and the URB
  has drained.
- A successful configuration/alternate selection followed by the mandatory
  HCD endpoint-reset operation, a successful clear-halt transaction, or a
  successful full device reset establishes an unhalted endpoint.
  Schedule-only endpoint re-enable during rollback preserves the prior
  toggle/halt state.  Cancellation, timeout, ordinary I/O error, or a failed
  recovery does not silently clear the latch.
- HCD cancellation may still use Stop Endpoint, Reset Endpoint, or Set TR
  Dequeue solely to prove retirement.  Those ownership commands never clear
  the USB-core halt latch.

The latch is set before entering a class callback and is checked again after
interface admission, under the same interface-I/O exclusion used by selection
transactions.  This prevents a callback or another CPU from slipping a new TD
between STALL publication and recovery.

## Endpoint clear-halt transaction

`drv_usb_endpoint_clear_halt()` accepts only a retained bulk or interrupt
endpoint in the active alternate of the active configuration.  A stale,
endpoint-zero, control, or isochronous endpoint returns `EINVAL` or `ENODEV`
without a wire request.

The transaction order is fixed:

1. Enter a device binding transaction, then pin the endpoint's effective
   binding owner through the ordinary binding-submitter and binding gate
   admission.  The shared transaction gate alone does not exclude explicit
   detach; the owner pin prevents detach from passing the operation.
2. Acquire the device selection gate without waiting.
3. Empty-close the target interface I/O gate.  This proves that no URB on the
   target interface is still admitted; otherwise return `EBUSY`.
4. Acquire endpoint-zero control admission without waiting; otherwise return
   `EBUSY`.
5. Send exactly this zero-length standard request:

   ```text
   bmRequestType = 0x02  (host-to-device, standard, endpoint)
   bRequest      = 1     (CLEAR_FEATURE)
   wValue        = 0     (ENDPOINT_HALT)
   wIndex        = bEndpointAddress, including direction
   wLength       = 0
   ```

6. Only after the device accepts that request, invoke the HCD endpoint-reset
   callback.
7. Only after the HCD callback succeeds, clear the core halt latch and reopen
   admission.

The operation is deliberately not an early-return no-op when the core latch is
clear.  BOT reset recovery is allowed to clear both endpoints when the class
protocol requires it even if the host did not observe a STALL on both.

Error ordering is also fixed:

- Validation, lifecycle, or admission errors have no wire or HCD side effect.
- A wire `EPIPE` leaves the endpoint latched and returns the error; the device
  is still in a known state.
- A pre-enqueue control-request failure has no device-side effect and unwinds
  normally.  Once the HCD has accepted the clear request, a timeout or
  transport error is ambiguous: the device is quarantined, its submit gate
  remains closed, and the original error is returned.
- Once the wire request succeeds, an HCD endpoint-reset failure leaves the
  latch set, retains all HCD-owned ring/schedule state, quarantines the device,
  and returns the HCD error.  The core must never publish a half-recovered
  endpoint.
- Disconnect or terminal shutdown wins by closing lifecycle admission.  The
  recovery operation returns `ENODEV` and does not reopen a gate owned by the
  teardown path.

## Frozen HCD endpoint-reset contract

Add this mandatory internal callback to `struct drv_usb_hcd_ops`:

```c
int (*endpoint_reset)(struct drv_usb_hcd *hcd,
    struct drv_usb_endpoint *endpoint);
```

Every registered HCD supplies the callback, including a no-op implementation
only when the HCD can prove that no additional host-side state exists.  HCD
registration rejects an absent callback.  The existing endpoint
enable/disable pair rule remains unchanged.

The USB core calls `endpoint_reset` only after a confirmed device-side event
which resets endpoint state: a successful clear-halt, successful configuration
or alternate selection, or successful full port/device reset.  Target
interface I/O is empty and closed.  The HCD does not send the USB request.
`endpoint_enable` only restores scheduling resources and therefore preserves
the prior toggle/ring state when a failed selection transaction rolls back.
Zero from `endpoint_reset` means the endpoint is ready for a new transfer and
its data-toggle/ring state agrees with the USB reset rules.  A nonzero result
must retain every uncertain hardware owner and DMA resource.

### xHCI

- Move normal endpoint recovery out of the unconditional enqueue path and
  into `endpoint_reset`.
- Keep endpoint-zero's existing implicit checked recovery because control
  STALL is intentionally not core-latched; every nonzero endpoint must be
  Running before enqueue and may recover only through the explicit callback.
- Hold a per-endpoint publication barrier from hardware STALL observation
  until the USB core has latched it, so another TD cannot enter that window.
- Reject reset while the endpoint has an active request, another endpoint
  recovery owns it, the device is quiescing, or the controller is stopping.
- Reuse the checked state machine: Halted issues Reset Endpoint, rereads the
  output context, then Stopped/Error issues Set TR Dequeue to the software
  producer index and current producer cycle.  Running is an idempotent
  success, which is required by a preventive BOT clear.
- Keep the bounded state/action retry rule and completion-code 19 state
  reread.  Never repeat a command against an unchanged state/action pair.
- Command failure leaves the ring and its producer state retained.  A new TD
  is not published and the core quarantines the device.
- Ordinary enqueue requires a Running endpoint and never turns an unconfirmed
  device-side halt into a host-only recovery.
- Root-port reset must reject a connection-status edge before or during reset;
  it must not acknowledge away evidence of a fast detach/reinsert.

### UHCI and EHCI

- `endpoint_reset` establishes DATA0 in the endpoint HCD-private toggle only
  after the core has proved that the target endpoint has no active owner.
- Endpoint enable is schedule/resource-only.  It preserves the retained toggle
  during failed configuration/alternate rollback.  The core separately calls
  `endpoint_reset` after a successful configuration/alternate selection or
  full-device reset, which then establishes DATA0.
- A partially completed stalled transfer may temporarily leave the retired
  hardware-derived toggle in HCD-private state, but the core halt latch makes
  it unusable until the successful clear transaction replaces it with DATA0.
- Preserve the p016 rules for successful, short, timed-out, and cancelled
  non-stalled transfers.  Recovery must not regress UHCI frame-boundary or
  EHCI Async Advance retirement.

## Frozen direct-root device-reset contract

The initial `drv_usb_device_reset()` scope is intentionally conservative.

- Only a non-root device directly attached to the HCD root hub is supported.
  A future hub child returns `ENOTSUP` before any side effect.
- The active configuration may have zero effective binding owners or one
  effective owner.  An interface claimed by another interface counts as the
  claimant's owner, so a single NCM-style owner and its claimed sibling are
  one owner.  Two independent bound function owners return `ENOTSUP` before
  any side effect.
- The trusted class-driver contract requires the one effective owner to call
  from its serialized worker after publishing a private resetting state and
  draining all of its URBs.  The core verifies that every active interface
  I/O gate, endpoint-zero control gate, and device HCD-URB count is empty.
- The reset retains the core `drv_usb_device`, configuration, interface,
  endpoint, claim, driver, and driver-data objects.  It restores standard USB
  address/configuration/alternate state, but not class-specific state such as
  HID Report Protocol, HID idle, CDC packet filters, or class keys.  The one
  owner must restore its class state before clearing its private resetting
  state and rearming traffic.
- No USB-driver reset callback is added in this Phase.  Supporting multiple
  independent function owners requires a separately designed prepare/complete
  notification transaction.

The nonblocking preflight and destructive sequence are fixed:

1. Acquire the global USB topology gate before closing any device-local gate.
   This is the same outermost ordering used by root enumeration and prevents a
   root worker from racing address-zero traffic or inverting the topology/
   device lock order.  Snapshot the core device generation and the direct-root
   port connection generation while this gate is held.
2. Empty-close the device binding-transaction gate, then acquire the selection
   gate.  Failure reopens what this invocation acquired, releases the topology
   gate, and returns `EBUSY`.
3. Verify the direct-root and effective-owner rules.
4. Empty-close every active-interface I/O gate and acquire endpoint-zero
   control admission.  Any active submit, callback, control URB, selection,
   binding, detach, or shutdown owner returns `EBUSY` or `ENODEV` before the
   destructive boundary.
5. Disable the active HCD endpoints with the checked configuration rollback
   rules.  Failure before hardware-device quiesce re-enables only endpoints
   disabled by this invocation and returns the original error; rollback
   failure quarantines the device.
6. For an HCD with device state, call checked `device_quiesce` and then
   `device_disable`.  xHCI thereby retires every request/completion/recovery,
   issues checked Disable Slot, and releases the old slot/rings/contexts only
   after hardware ownership is proven.  Legacy HCDs have no per-device object.
7. Reset the root port using the controller callback or the existing generic
   root-hub request sequence.  Re-read connection state while the topology gate
   is still held.  A disconnect, a connect-status edge, or any change in the
   captured port/device generation is `ENODEV`: quarantine the old object and
   never restore its address, configuration, binding, or endpoint state onto a
   newly attached physical device.
8. Recreate HCD device state when required, restore the existing USB address
   through the HCD address callback or a wire `SET_ADDRESS` from address zero,
   and observe the bounded recovery delay.
9. Restore the previously selected configuration and every previously active
   nonzero alternate using standard requests, enable their scheduling
   resources, then invoke endpoint reset to establish DATA0/unhalted state.
10. Publish Address/Configured state, reopen exactly the device-local gates
    acquired by this invocation, and release the topology gate last.  The
    retained binding objects do not change generation only when the captured
    physical connection and core device generations are unchanged.

Calling checked HCD device quiesce begins the destructive boundary.  A port
reset, HCD-device recreation, address, configuration, alternate, or endpoint
enable failure after that point cannot restore the old physical state.  The
core keeps uncertain HCD ownership, closes submission, quarantines the device,
and returns the first error.  It must not reconnect the old slot, ring, or
toggle state.  A later disconnect/controller teardown owns final retirement.

## USB Mass Storage migration

Replace both Mass Storage recovery sites with
`drv_usb_endpoint_clear_halt()`:

- a data-stage `EPIPE` clears the affected endpoint through the common ordered
  transaction before continuing to the CSW; and
- BOT reset sends the class reset request first, then clears bulk-IN and
  bulk-OUT through the common API in protocol order.

Remove the private request constant/helper and all class-side calls to
`drv_usb_endpoint_set_hcd_data()`.  A failure at either endpoint follows the
common quarantine/retention contract and is not hidden by a second raw toggle
write.  Preserve recovery under USB-backed swap pressure: the common core owns
a preallocated endpoint-zero recovery URB per device, marks it
`DRV_USB_URB_RECLAIM_SAFE`, and releases it centrally before final device
lifecycle-reference checks.  Endpoint recovery must not allocate after the
class has entered its error path.

## Detailed procedure

1. Add the core endpoint halt state, callback-before-rearm ordering, submission
   rejection, public class-driver API, and mandatory HCD operation.  Update
   fake-HCD registration validation without weakening existing endpoint-pair
   checks.
2. Implement the ordered endpoint clear-halt transaction with effective-owner
   pinning, binding, selection, interface-I/O, endpoint-zero, disconnect, and
   shutdown exclusion.  Use a core-preallocated reclaim-safe recovery URB,
   distinguish pre-enqueue from accepted ambiguous failures, and audit every
   unwind so it reopens only gates acquired by that invocation.
3. Move xHCI recovery to the explicit HCD callback.  Keep checked command-event
   identity, state rereads, ring producer/cycle ownership, failure retention,
   callback drain, and controller-stop barriers.
4. Implement UHCI/EHCI DATA0 reset independently from schedule-only endpoint
   enable while preserving their p016 hardware-retirement, failed-selection
   rollback, and normal toggle-continuity rules.
5. Implement the direct-root, single-effective-owner device reset using the
   existing checked HCD teardown/recreate, root-port reset, address, and
   configuration restore primitives.  Add no driver callbacks or user UAPI.
6. Migrate USB Mass Storage to the common operation and remove all class access
   to HCD-private endpoint state.
7. Add focused production-source fixtures, sanitizer/analyzer gates, configured
   builds, and non-fault-injected QEMU controls.  Re-run every named USB
   ownership and storage regression.
8. On completion, update `ws006-p008` only in a later authorized planning edit
   to point to p032 as a satisfied prerequisite.  This Phase does not cross
   that downstream implementation boundary.

## HW-T26 focused verification

Add `plan/ws004-hardware/tests/usb-recovery-contract-test.c` and
`run-usb-recovery-contract-test.sh`.  The runner links the production USB core
and the relevant production lifecycle helpers rather than a parallel recovery
implementation.

The focused fixture must cover:

- STALL latching before callback entry, immediate callback rearm returning
  `EPIPE` with zero HCD enqueues, and worker recovery only after callback return
  and URB drain;
- the exact standard setup packet and wire-before-HCD-before-unlatch ordering;
- repeated preventive clear, wire STALL, timeout, disconnect, HCD failure, and
  exact gate/latch/quarantine results;
- active target and sibling-interface URBs, endpoint-zero control ownership,
  alternate/configuration selection, detach, and shutdown races without a
  duplicate callback or leaked HCD/DMA owner;
- xHCI Running, Halted, Stopped, Error, Disabled, completion-code 19, unchanged
  state/action, producer wrap, cycle-bit, command failure, and retained-ring
  cases, including proof that enqueue without a completed clear-halt
  transaction emits no recovery command;
- UHCI and EHCI first-post-clear and successful-selection DATA0, failed-
  selection rollback preserving the old toggle, plus unchanged success,
  short-transfer, timeout, and cancellation toggle continuity;
- USB-backed-swap recovery with no allocation in the endpoint error path and
  balanced lifetime of the core-preallocated reclaim-safe recovery URB;
- device-reset preflight with no side effects, exact successful teardown/reset/
  address/configuration/alternate/re-enable sequence, stable core objects and
  bindings, one owner with claimed siblings, and rejection of multiple
  independent owners and hub children;
- root-worker contention against reset, proof of the fixed topology-first lock
  order, and disconnect/reinsert during reset with a changed connection/device
  generation returning `ENODEV` or quarantine without applying the old address,
  configuration, binding, or endpoint state to the replacement device;
- injected failure at endpoint disable/rollback, HCD quiesce, port reset,
  device recreate, address, configuration, alternate, and endpoint enable,
  proving either pre-destructive rollback or post-destructive quarantine and
  exact resource retention; and
- BOT data STALL and class reset recovery using the common API, with a source
  assertion that USB Mass Storage no longer reads or writes endpoint HCD data.

Run the new fixture in ordinary, ASan/UBSan, and compiler-analyzer modes.  Then
run these existing regressions in their declared modes:

- USB function model and binding transactions;
- concurrent xHCI URBs and xHCI model;
- legacy-HCD request retirement and the p031 legacy concurrency/hotplug test;
- USB Storage SCSI/BOT, USB-root continuity, and overlay-write regression; and
- CDC NCM binding/lifecycle regressions so the shared USB API change does not
  alter unrelated composite ownership.

Build gates are the default `make -j16`, configured amd64 UEFI/xHCI and i386
PC/AT legacy-HCD objects, and the p031-supported configurations.  Runtime
controls use disposable images for one q35/xHCI USB-root boot and the p031
UHCI/EHCI QEMU runs with concurrent storage.  If QEMU exposes no stable public
endpoint-STALL or reset fault injection, those failures remain focused
production-source evidence and the QEMU ledger says `not_injected`; a normal
boot is not reported as injected recovery evidence.

`make check` and `.internal/` are not used.  `git diff --check` must pass.

## Completion conditions

- One class-driver API performs ordered device-side clear-halt and host-side
  ring/toggle reset on xHCI, UHCI, and EHCI.
- A stalled bulk or interrupt endpoint cannot reach any HCD again until the
  complete recovery transaction succeeds.
- xHCI submits no implicit recovery command before device-side confirmation;
  failed Reset Endpoint/Set TR Dequeue retains its ring and DMA ownership.
- UHCI/EHCI start the first post-clear transfer at DATA0 without regressing
  p016 short, cancellation, or retirement continuity.
- `drv_usb_device_reset()` succeeds for a drained, directly attached device
  with zero or one effective owner, retains the core binding objects, restores
  standard address/configuration/alternate state, and quarantines every
  ambiguous post-destructive failure.
- USB Mass Storage uses no HCD-private recovery state, and its BOT recovery and
  all named USB ownership/storage regressions pass.
- HW-T26 ordinary, sanitizer, analyzer, configured-build, and declared QEMU
  controls pass with exact evidence boundaries.
- The general recovery prerequisite of `ws006-p008` is satisfied without
  claiming that USB HID, hub reset, or multi-owner device reset is implemented.

## Reconsideration boundary

Stop and mark the Phase `uncleared` rather than silently enlarging it if:

- the p008 target requires reset of a composite device with multiple
  independent function owners; that needs a separately approved
  prepare/complete or reset-session driver contract;
- a device behind a USB hub must be reset; that needs a hub-port ownership and
  reset Phase rather than use of the root-port primitive;
- a supported HCD cannot prove endpoint/ring/toggle or device-DMA retirement
  without resetting unrelated devices or the whole controller;
- recovery must run synchronously inside the completing URB callback;
- a user-visible UAPI, vendor-specific quirk, class-output feature, or new
  public driver ABI beyond the two frozen operations becomes necessary; or
- QEMU-only normal boots are the only evidence available for a fault path.  Do
  not relabel absence of an injected failure as recovery coverage.

In each case retain the exact known owner or quarantine the device, record the
missing decision/evidence, and move the broader mechanism to a new Phase.
