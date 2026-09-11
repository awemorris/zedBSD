# WS004 Phase 012: network-device hotplug and shutdown lifetime

Last updated: 2026-08-29

Phase ID: `ws004-p012`

Status: completed (`q027`)

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

## Actual result

- `net_device` now separates LIVE, REMOVING, and GONE publication,
  destroy-pending state, and final last-reference reclamation.  A concurrent
  removal joins the first removal as a barrier instead of returning early, and
  the optional driver `release` callback runs only after synchronous `close`
  and all external references have retired.
- Poll admission and rescheduling require a live, open, non-transitioning
  device.  Final close clears pending work and joins an in-flight poll before
  returning.  Removal likewise joins open, close, and poll activity before the
  bus owner may release resources.
- Removal requested while IRQs were already disabled returns `EWOULDBLOCK`
  without changing registry or lifecycle state; existing bus drivers retain
  ownership on that result and may retry from thread context.
- Route, IPv4-interface, and ARP identities retain explicit device references
  and are purged before a removed registry slot may be reused.  New route,
  socket, transmit, receive, and poll paths reject a GONE device.
- Carrier and `NET_DEVICE_RUNNING` publication are serialized by the network
  device layer; loopback and DP8390 use this API instead of mutating running
  flags directly.
- The common orderly-stop boundary is now network close/drain, USB class-driver
  detach, USB device/HCD quiesce and stop, then PCI shutdown.  Both system-device
  and kernel-shell halt/reboot paths cross the boundary.
- Terminal network shutdown closes admission before draining the registry,
  removes every live device regardless of its open-reference count, and joins
  removals that were already in progress.  GONE is published only after the
  driver callback barrier, all open-derived references, and every network
  identity purge have retired.
- USB terminal shutdown and normal port removal both close every independent
  interface before device DMA quiesce.  The first detach error is retained
  while later functions are still stopped; any detach/quiesce failure preserves
  the affected graph and suppresses final release/HCD stop.

## Evidence

- `run-net-device-hotplug-test.sh`: net-device lifetime/race, ARP purge, and
  IPv4 interface reuse fixtures all pass, including close-versus-poll,
  gone-versus-open/close/gone, IRQ-disabled deferral, concurrent shutdown with
  an already-removing device, multi-open shutdown drain, and repeated slot
  reuse.  The strengthened net-device fixture also passes 500 consecutive
  executions.
- `run-system-shutdown-order-test.sh`: common shutdown order and both reachable
  halt/reboot entry points pass.
- Production USB-core fake-HCD fixture: `1280 checks passed`.  It fixes terminal
  order `detach -> device quiesce -> HCD quiesce -> stop`, verifies stop
  suppression after detach/device-quiesce failure, and fixes hot-unplug order,
  ownership retention/release, and finalize suppression on a composite device.
- Address/undefined-behavior sanitizer runs pass for the three network fixtures,
  common shutdown fixture, and combined USB-core fixture.
- Focused freestanding `-Werror` builds pass for amd64, i386 PC/AT, and PC-98;
  the combined USB review also passes full amd64 and i386 PC/AT builds plus the
  xHCI, USB-storage SCSI, and URB-publication regressions.
- Scoped `git diff --check` passes.  Neither `make check` nor `.internal/` was
  used.
