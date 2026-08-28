# WS004 Phase 011: concurrent xHCI endpoint URBs

Last updated: 2026-08-29

Phase ID: `ws004-p011`

Status: pending (`q027`)

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
  explicit and testable.
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

## Completion conditions

- Three endpoint transfers plus an independent storage request coexist and
  complete in arbitrary order in the model/production-source fixture.
- Canceling one request leaves the others operational.
- Device detach and global shutdown leave no callback, DMA, TRB, or request
  ownership dangling; unsafe late-DMA cases remain quarantined.
- Existing xHCI, USB-storage, USB-root regressions and relevant configured
  builds pass.
- `git diff --check` passes without `make check` or `.internal/`.

## Reconsideration boundary

Stop if retaining the storage reclaim guarantee requires changing the public
swap/block contract or an unbounded allocation. Record the exact reserve and
ownership conflict rather than weakening boot-storage reliability.
