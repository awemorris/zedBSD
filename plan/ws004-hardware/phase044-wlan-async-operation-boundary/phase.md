# WS004 Phase 044: asynchronous WLAN operation boundary

Last updated: 2026-09-04

Phase ID: `ws004-p044`

Status: complete (`q071`); asynchronous kernel ownership, minimal read-only
`AF_ROUTE`/`RTM_IFINFO` events, focused gates, and shared physical acceptance
pass

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

Historical implementation: [WS004 p030](../phase030-wlan-lifecycle-hardware-hardening/phase.md)

## Objective

Correct the WLAN ownership boundary exposed by the post-p043 design review.
The kernel provides asynchronous scan and single-attempt connection state,
protocol timers, carrier truth, and link-loss notification.  It must not own a
30-second high-level reconnect loop.  `/sbin/wifi`, and only that finite
userspace command, owns the 30-second scan/select/connect retry policy used by
both direct administration and `networkd`.

P030 remains valid historical evidence for rekey, controlled-port ordering,
USB/device lifecycle, checked cleanup, and its deterministic fixtures.  Its
automatic same-BSS 0/1/2/4/8-second reconnect policy is deliberately
superseded by this Phase rather than retained as a compatibility path.

## Frozen responsibility split

### Kernel

- `SIOCSWLANSCAN` starts or stops a scan and returns promptly.  Driver work,
  channel dwell, and result publication continue asynchronously; callers use
  `SIOCGWLANSCAN`/`SIOCGWLANBSS` to observe a generation.
- `SIOCSWLANCONNECT` admits one connection generation and returns promptly.
  Authentication, association, the WPA2 four-way handshake, retransmission of
  protocol frames, and finite per-transition timers remain asynchronous kernel
  protocol work.  The kernel does not rescan or begin another connection
  generation after that attempt becomes terminal.
- Link loss immediately closes the controlled port, lowers carrier, retires
  TX, association, PTK/GTK/nonces and reconnect-only PMK state, publishes a
  terminal disconnected/failed generation, and emits an interface link event.
  It never schedules an automatic same-BSS retry.
- Explicit disconnect/down, replacement, removal, and shutdown cancel the
  current asynchronous generation and make every later completion stale.

### `/sbin/wifi`

WS005 p010 consumes the above primitives.  One `wifi INTERFACE connect SSID
PASSPHRASE` invocation owns one monotonic 30-second operation deadline.  It
starts or joins asynchronous scanning, selects a supported BSS, submits one
asynchronous connection generation, observes status, and repeats only
retryable scan/connection failures while time remains.  It does not make an
ioctl sleep for 30 seconds and does not leave a userspace process resident
after success or terminal failure.

### `networkd`

WS005 p011 receives a link-down event only for a connection which previously
succeeded through `net` -> `networkd`.  It invokes the same finite
`/sbin/wifi` connect command once.  Therefore the daemon observes a 30-second
recovery attempt without implementing a second timer/backoff loop of its own.

## Link-event contract

Add the minimum BSD-style routing-socket notification needed for generic
interface lifecycle events instead of a WLAN-private blocking ioctl:

- `socket(PF_ROUTE, SOCK_RAW, 0)` supports a bounded,
  fixed-width versioned `RTM_IFINFO` record for carrier/running and removal
  transitions;
- each record carries interface index, device-generation identity, event
  sequence, current flags, and transition kind, with reserved fields required
  to be zero;
- `net_device_set_carrier()` emits only on an actual carrier transition, and
  device removal emits one terminal record before identity retirement;
- socket receive and `poll()` are nonblocking-capable, bounded, and wake on a
  queued event; overflow is explicit and forces a full interface-status
  resnapshot rather than silently losing truth;
- `networkd` opens the event socket before taking its initial status snapshot,
  then reconciles queued sequence numbers, so a transition cannot be lost
  between snapshot and subscription; and
- the event is only a wakeup/identity hint.  `networkd` re-reads the canonical
  WLAN and interface status before acting.

This Phase does not implement route mutation through `AF_ROUTE`, netlink, a
general wireless event protocol, or a blocking `SIOCGWLAN*` wait operation.

## Implementation sequence

1. Extend focused p030 fixtures to assert prompt scan/connect ioctl return,
   asynchronous generation progress, and exactly one terminal result.
2. Remove the common-WLAN automatic reconnect scheduler, delayed attempts,
   reconnect scan generation, and PMK-preservation path while retaining
   protocol-local retransmission/rekey and checked cleanup.
3. Make link loss publish terminal L2 state immediately and scrub all material
   which existed only to support in-kernel reconnect.
4. Add the minimal route-event socket and hook actual carrier/removal
   transitions into it without driver-specific callbacks.
5. Add loss/overflow/subscription-race, detach/reuse, cancellation, and
   shutdown fixtures, then rerun the retained p027--p030 and USB/storage
   regressions.

## Verification

- Scan start/stop and connect admission return promptly while their generation
  advances asynchronously under status polling.
- A failed connect produces one terminal generation and no kernel-created scan
  or second connect generation, even after more than 30 synthetic seconds.
- Link loss after authorization immediately closes carrier/controlled port,
  emits one matching event, clears reconnect-only secret state, and remains
  disconnected until a new userspace connect request.
- Protocol-local authentication/EAPOL retransmissions and connected-state GTK/
  pairwise rekey continue to pass; they are not misclassified as high-level
  connection retries.
- Event delivery handles queue limits, poll wakeups, listener startup races,
  close, device removal, ifindex reuse, and daemon restart without stale
  identity aliasing or an unbounded allocation.
- Focused ordinary/sanitizer/analyzer/race gates, configured amd64/i386 builds,
  retained RTL8822BU/AX211 fixtures, USB-storage concurrency, and one bounded
  QEMU boot pass.  Do not run aggregate `make check`.

## Completion conditions

- The kernel contains no automatic 30-second same-BSS reconnect policy.
- Scan and one-attempt connect remain asynchronous kernel operations with
  truthful observable generations and bounded protocol timers.
- Generic carrier/removal notifications are usable by a poll-driven
  `networkd`, and no WLAN-private blocking syscall is introduced.
- P010 can implement the sole 30-second userspace retry loop and p011 can
  trigger exactly one such command after an established managed link drops.

## Interruption and resumption

If a radio operation cannot be made asynchronously observable without changing
the public WLAN ABI, stop with the exact missing field/event and update this
Phase before changing the header.  If a route-event socket requires broader
route-control semantics, retain only the fixed read-only `RTM_IFINFO` subset;
do not silently expand this Phase into a complete routing-socket facility.
