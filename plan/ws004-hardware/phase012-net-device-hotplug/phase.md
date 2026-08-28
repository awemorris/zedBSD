# WS004 Phase 012: network-device hotplug and shutdown lifetime

Last updated: 2026-08-29

Phase ID: `ws004-p012`

Status: in-progress (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make `net_device` lifetime safe for a removable USB network adapter and expose
a synchronized carrier state suitable for CDC NETWORK_CONNECTION events.

## Frozen contract

- Device removal immediately prevents new lookup, configuration, route, and
  transmit use, while already referenced RX work may finish safely.
- Final slot reclamation occurs automatically after the last external
  reference; repeated reconnect cannot exhaust the fixed registry.
- ARP entries and routes that retain a removed device are purged before its
  slot can represent another interface.
- Carrier/running state changes use a synchronized kernel API, not direct flag
  mutation by drivers.
- USB network shutdown order is driver close/shutdown, URB cancel and callback
  drain, HCD quiesce, then memory release. Existing boot/halt paths invoke the
  registered shutdown boundary where required.

## Planned work

1. Add gone/destroy-pending state and last-reference reclamation.
2. Add ARP purge and verify route/address cleanup before slot reuse.
3. Add carrier/running update and query operations with bounded notification
   semantics.
4. Make close/shutdown ownership capable of waiting for asynchronous producer
   retirement without freeing callback-visible state.
5. Add fixtures for queued RX during detach, stale ARP, more than eight
   reconnects, carrier transitions, close/cancel races, and shutdown ordering.

## Completion conditions

- A referenced removed device is inaccessible to new users and is reclaimed
  exactly once after its last reference is released.
- More than the registry capacity in attach/detach cycles succeeds without a
  stale ARP/route/address alias.
- Carrier events publish deterministic running state.
- Shutdown ordering has focused evidence and existing network fixtures/builds
  pass.
- `git diff --check` passes without `make check` or `.internal/`.

## Reconsideration boundary

Stop if safe asynchronous close requires changing every existing network
driver's ownership convention. Record a bounded adapter-specific bridge rather
than silently imposing a new unreviewed public UAPI.
