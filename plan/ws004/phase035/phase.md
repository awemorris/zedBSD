# WS004 Phase 035: USB same-endpoint multi-URB rings

Last updated: 2026-09-01

Phase ID: `ws004-p035`

Status: planned/deferred; nonblocking; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Extend the generic USB and xHCI ownership contract from one active URB per
endpoint to a bounded queue of active URBs on one endpoint. This is a future
throughput and latency capability for devices such as WLAN; it is not required
for the scan-only RTL8822BU milestone, which deliberately uses one persistent
bulk-IN URB and poll-context drain/rearm.

## Scope

- advertise a separate HCD capability for same-endpoint queuing without
  changing UHCI/EHCI behavior;
- define a fixed per-endpoint queue bound and checked submission backpressure;
- associate every xHCI TD/event with exactly one admitted URB across ring wrap,
  short packets, stalls, timeouts, cancellation, and late events;
- support request-local cancellation when possible and checked endpoint
  Stop/Set TR Dequeue retirement when hardware requires it;
- keep endpoint disable, configuration change, device reset, detach,
  quarantine, and shutdown as synchronous ownership barriers;
- preserve fairness between endpoints and devices so a WLAN RX ring cannot
  starve USB storage, HID, or another controller; and
- retain the existing one-active-URB rule on HCDs which do not advertise the
  new capability.

## Verification

Add production-source host/model tests for queue-full admission, in-order and
out-of-order terminal events where the controller permits them, per-request
cancel, endpoint stop/restart, wraparound, stale/duplicate/foreign events,
short transfer, stall recovery, detach with blocked completion, and concurrent
storage/HID traffic. Run ordinary, sanitizer, analyzer, configured amd64/i386,
and xHCI QEMU regressions without using `.internal/` or aggregate `make check`.

## Completion conditions

- one endpoint can safely own the declared bounded number of simultaneous URBs
  on a supporting xHCI controller;
- every accepted request has exactly one terminal publication and release;
- failure or teardown either proves complete retirement or retains the full
  graph for checked retry; and
- legacy HCDs and existing one-request drivers remain unchanged.

## Resume condition

Queue this Phase only when measured WLAN or another concrete device workload
requires more than one active request on one endpoint. It is not a dependency
of `ws004-p028`, `p029`, or the first WLAN acceptance.
