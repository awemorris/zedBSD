# WS004 Phase 011: concurrent xHCI endpoint URBs

Last updated: 2026-08-29

Phase ID: `ws004-p011`

Status: completed (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Replace xHCI's controller-wide single active request with explicit concurrent
request ownership sufficient for endpoint zero, CDC notifications, persistent
bulk RX, bulk TX, and an independent USB-storage device.

## Frozen contract

- Transfer-event ownership is resolved by slot, endpoint/DCI, and submitted TRB
  identity rather than one controller-global pointer.
- At least one request per endpoint may be active concurrently; unsupported
  additional queue depth returns `EBUSY` without corrupting rings.
- Cancel affects only the requested transfer. Device quiesce drains or safely
  quarantines every request owned by that device; controller teardown covers
  all devices.
- Out-of-order endpoint completions cannot complete, free, or publish another
  request.
- Persistent network RX/notification traffic must not consume the emergency
  DMA reserve needed by reclaim-time USB-storage I/O. Reserve eligibility is
  explicit through `DRV_USB_URB_RECLAIM_SAFE` and testable. The public bounded
  reserve maximum is 8 KiB. An explicitly eligible oversized request returns
  `EMSGSIZE` rather than falling back to allocation; an ordinary request never
  consumes the reserve.
- `drv_usb_urb_setup_control_flags()` carries explicit control-transfer flags;
  the existing `drv_usb_urb_setup_control()` remains a flags-zero wrapper.
  USB storage marks its control, CBW, CSW, and data transfers reclaim-safe only
  when their transfer length is at most 8 KiB. Larger storage I/O and ordinary
  CDC NCM traffic use the dynamic path.
- The reclaim reserve is intentionally one controller-wide eligible request.
  Every USB-storage stage at or below 8 KiB is eligible (there is no separate
  VM-reclaim context), so a simultaneous second eligible storage stage returns
  `EBUSY` without changing either endpoint owner. Large storage data is
  ordinary/dynamic; its sequential CBW and CSW remain eligible. This is a
  deliberate bounded limitation, not a guarantee that every HCD or every
  simultaneous storage device can allocate without waiting.
- `DRV_USB_HCD_CAP_CONCURRENT_URBS` is the public behavior bit for an HCD that
  accepts independent endpoint owners. xHCI advertises it; the unchanged
  controller-global UHCI/EHCI implementations do not. Function drivers query
  it through `drv_usb_device_hcd_capabilities()` and never identify an opaque
  HCD through names, ops tables, or private data.
- `drv_usb_urb_drain(urb, timeout_ms)` joins an asynchronous URB without
  cancelling it. Success requires both terminal status and released HCD
  ownership after callback return; zero waits indefinitely and a nonzero
  timeout returns `ETIMEDOUT` without releasing or changing the caller-owned
  URB/callback graph. A callback must not drain its own URB.
- A cancelled endpoint remains owned and rejects reuse until Stop Endpoint and
  Set TR Dequeue complete successfully (with Reset Endpoint when required), or
  until the whole controller is proven DMA-quiescent. A software-only unlink
  is forbidden. Transfer Events must match slot, DCI, and an exact TRB in the
  submitted, wrap-aware TD; a late or mismatched event cannot complete a new
  owner.
- Controller shutdown may software-drain retained requests only after
  `HCHalted`, PCI bus-master disable, and IRQ drain all complete. It publishes
  `DISCONNECTED` with release/acquire ordering, invokes callbacks outside the
  ownership lock, and requires every endpoint owner to be null before DMA
  resource release.
- UHCI/EHCI remain unchanged and NCM must reject them safely while their HCD
  contract remains single-flight.

## Planned work

1. Replace global active-request state with bounded slot/endpoint ownership.
2. Route transfer events to the exact request and retain existing release/
   acquire terminal publication.
3. Make dequeue, Stop Endpoint, Set TR Dequeue, device quiesce, and controller
   quiesce operate on the correct request sets.
4. Separate ordinary persistent-transfer allocation from reclaim-safe reserve
   ownership.
5. Add fixtures for notification/RX/TX/control concurrency, out-of-order
   completion, isolated cancel, disconnect, a second storage device, reserve
   exhaustion, and shutdown.

## Verification procedure

1. Run `tests/run-xhci-concurrent-urbs-test.sh` with
   `TMPDIR=build/q027-tmp`. It executes all 120 completion orders, malformed
   event identities, isolated cancel and late-event ownership, device/global
   drains, reserve eligibility, the HCD capability query, callback-aware URB
   drain, ASan/UBSan, GCC `-fanalyzer`, and a production source contract audit.
2. Run the existing xHCI model, USB publication, USB-storage SCSI, and USB
   function-model fixtures.
3. Compile the production amd64 and configured i386 xHCI/USB objects, then run
   `make -j16` without `make check`.
4. Boot one disposable USB-root image with `qemu-system-x86_64` and confirm
   login plus absence of xHCI, USB-storage, loop1, heap, or panic markers.

## Completion conditions

- Three endpoint transfers plus an independent storage request coexist and
  complete in arbitrary order in the model/production-source fixture.
- Canceling one request leaves the others operational.
- xHCI alone advertises concurrent endpoint ownership, and an asynchronous URB
  drain cannot cross a callback which has not returned and dropped HCD
  ownership.
- Device detach and global shutdown leave no callback, DMA, TRB, or request
  ownership dangling; unsafe late-DMA cases remain quarantined.
- Existing xHCI, USB-storage, USB-root regressions and relevant configured
  builds pass.
- `git diff --check` passes without `make check` or `.internal/`.

## Reconsideration boundary

Stop if retaining the storage reclaim guarantee requires changing the public
swap/block contract or an unbounded allocation. Record the exact reserve and
ownership conflict rather than weakening boot-storage reliability.

## Result

Completed on 2026-08-29.

- xHCI now owns an active request per slot/DCI endpoint and claims Transfer
  Events by exact, wrap-aware submitted TRB identity while the event lock is
  held. Different endpoints and devices can progress independently; a second
  request on one endpoint remains the explicit `EBUSY` boundary.
- Cancellation retains the endpoint owner until Stop Endpoint and Set TR
  Dequeue establish the hardware detach boundary. Device quiesce drains every
  endpoint it can safely detach; controller-wide software drain is permitted
  only after halt, PCI bus-master disable, and IRQ drain.
- Transfer completion is claimed under the event lock, queued, then dispatched
  outside command/event/active locks. Request/DMA storage is released before
  terminal callback publication, while USB HCD ownership remains until that
  callback returns.
- USB storage uses persistent synchronous URBs and marks control, CBW, CSW,
  and at-most-8-KiB data stages reclaim-safe. Larger data and ordinary NCM URBs
  use dynamic allocation. The documented single controller-wide reserve makes
  a simultaneous second eligible request fail safely with `EBUSY`.
- xHCI advertises `DRV_USB_HCD_CAP_CONCURRENT_URBS`; UHCI/EHCI remain
  single-flight. The opaque query and callback-aware `drv_usb_urb_drain()`
  provide the boundaries needed by the following NCM integration Phase.

Evidence:

- `run-xhci-concurrent-urbs-test.sh`: all 120 completion orders, exact event
  identity, isolated cancel, late-event/ring-reuse barrier, device/controller
  drain, reserve, HCD capability, and blocking-callback URB drain pass;
- production USB-core fixture: 1323 checks pass normally and with ASan/UBSan;
- xHCI concurrent model and USB-core fixture pass GCC `-fanalyzer`; production
  xHCI and USB translation units also pass the freestanding analyzer gate;
- existing xHCI arithmetic, USB-storage SCSI, URB publication, CDC NCM wire,
  and system-shutdown-order regressions pass;
- full `make -j16` amd64 and configured i386 PC/AT builds pass;
- one disposable `qemu-system-x86_64` USB-root boot reaches `login:` with no
  xHCI, USB-storage, loop1, heap, or panic failure marker;
- independent ownership/race review found no completion blocker, and scoped
  `git diff --check` passes without `make check` or `.internal/`.

The deliberate remaining limits are one reclaim-reserve owner per controller,
queue depth one per endpoint, and unchanged UHCI/EHCI single-flight behavior.
Physical CDC NCM interoperability remains the declared WS005 boundary.
