# WS005 Phase 001: USB NCM physical data-path isolation

Last updated: 2026-08-29

Phase ID: `ws005-p001`

Status: in progress (`q029`); automatic safe slice complete, physical check pending

Parent: [WS005 networking and WPA](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

With `ws004-p020` complete, apply the safe DHCP/diagnostic fixes, build one
candidate image, and give the generic
CDC NCM `ue0` path on the Dell Latitude 5320 and physical RTL8156 one bounded
combined carrier/static/DHCP acceptance without freezing.

This Phase instantiates the first bounded slice of NET-01 and NET-10. It starts
from a class driver which is already selected and bound; it does not reopen USB
configuration matching or add a Realtek VID-specific frontend without new
evidence.

The user has approved one physical check before CDC ECM. If that check still
fails after the deterministic automatic work, `ws004-p019` becomes the next
Queue item and establishes an independent CDC ECM path against QEMU `usb-net`
before any second physical-debug cycle. ECM is not part of q029 or this Phase's
authorized implementation slice.

## Physical evidence that opened this Phase

The accepted `ws004-p018` image selected the NCM configuration and published
the expected network device:

```text
usb0: device 1 port 3 0bda:8156 class 00 configuration=2 configured
usb-cdc-ncm: ue0 mac=6c:1f:f7:66:14:8a
usb0: device 1 interface 0 class 02/0d/00 driver=usb-cdc-ncm
usb0: device 1 interface 1 class 0a/00/01 claimed-by=0 driver=usb-cdc-ncm
```

`net show` listed `ue0`, satisfying p018. The first data-path observation was:

- `net dhcp ue0` ended with an `ue0: error` diagnostic;
- a static IPv4 address could be assigned, but peer ping did not complete; and
- the user observed behavior consistent with a freeze when inbound activity
  should have completed, although the exact IRQ-versus-worker boundary is not
  yet proven.

These observations are intentionally recorded as symptoms, not as a claim that
the xHCI interrupt handler is already known to be the fault site.

## Dependencies

- `ws002-p020`: `net` -> `networkd` -> `dhcpc` baseline and direct-ifconfig
  recovery path.
- `ws004-p012`: removable `net_device` and deferred polling lifetime.
- `ws004-p014`: integrated CDC NCM wire/driver and URB lifecycle.
- `ws004-p018`: physical Union-only NCM selection, bind, and `ue0` publication.
- `ws004-p020`: deterministic valid-sequence/resynchronization policy, bounded
  completion work, and packet-filter programming on open after the active
  alternate. Complete in q029.

`ws004-p019` is a conditional successor, not a q029 dependency: it is queued
next only if the single hardened physical check below fails.

## Static audit findings and discriminators

1. Static address ioctls prove only the configuration plane. They do not prove
   carrier, TX completion, RX completion, ARP, or IPv4 input.
2. An unconfigured interface is expected to appear `offline` before it is
   opened. DHCP opens the interface, but the zero-address DHCP broadcast path
   requires `UP|RUNNING|BROADCAST`; `RUNNING` depends on a valid CDC
   `NETWORK_CONNECTION` notification setting carrier.
3. An immediate DHCP `Network is down`-class failure points first to open,
   notification, or carrier publication. A failure only after the requested
   timeout means DHCP DISCOVER was admitted and points first to TX/RX, NCM wire
   parsing, or reply delivery. Preserve the exact child diagnostic and elapsed
   stage rather than reducing both cases to `error`.
4. xHCI completion invokes the short NCM completion callback, but the hard-IRQ
   path drains the completion ring without a per-entry budget and performs
   buffer reclamation plus callbacks there. A notification or RX completion
   storm can therefore amplify another defect into CPU starvation even though
   NTB parsing and protocol input themselves run in the network worker.
5. The current strict NTH16 parser begins with expected sequence zero, rejects
   every mismatch, and advances only after a completely valid NTB. The driver
   immediately rearms after parse failure, while poll accounting counts
   delivered frames rather than a malformed completion. `ws004-p020` now owns
   the approved deterministic correction: every fully valid sequence is
   delivered/resynchronized, malformed input preserves state, and every
   consumed completion is budgeted. Physical causality remains unclaimed until
   the q029 check.
6. RTL8156-family functions may repeat connection notifications and split a
   speed-change header and payload across transfers. The current notification
   parser accepts only exact 8-byte connection or exact 16-byte speed records.
   A fixed trace must establish the actual device behavior before adding any
   bounded reassembly or duplicate suppression.
7. The NCM attach path programs `SET_ETHERNET_PACKET_FILTER` before selecting
   the active data alternate. p020 moves this control request into each open,
   after the active alternate and before traffic URBs.
8. `dhcpc` snapshots an existing static address but does not clear it before a
   zero-address DHCP DISCOVER. The UDP path therefore selects the old static
   IPv4 source while BOOTP `ciaddr` remains zero. This deterministic transition
   bug must be fixed transactionally before DHCP is useful as an NCM oracle.
9. libc `strerror()` lacks at least `ENETDOWN` and `ETIMEDOUT`, so both an
   immediate carrier failure and a completed DHCP wait can print `Unknown
   error`. The safe diagnostic correction must make those outcomes distinct;
   broader `networkd` protocol redesign is not required for this check.

## Planned implementation

1. Complete `ws004-p020` and its focused/regression/build gates. Do not prepare
   a candidate image from a partially hardened NCM path.
2. Correct the static-to-DHCP transition transactionally. After snapshotting
   the prior address/mask/broadcast/flags and any default route owned by the
   interface, clear the configured IPv4 address
   before the first DISCOVER so the IPv4 source and BOOTP `ciaddr` are both
   zero. On any failure, restore the complete prior static and default-route
   state; on success, commit only the lease configuration. Use one total
   deadline for carrier and DHCP receive processing, including sustained
   irrelevant or invalid input. Cover clean, preconfigured-static, timeout,
   send failure, offer/ACK, and rollback cases with focused evidence.
3. Add the missing `strerror()` mappings for `ENETDOWN` and `ETIMEDOUT` and
   focused checks that preserve the immediate-versus-timeout distinction in
   the actual `dhcpc`/`net` diagnostic path.
4. Add bounded transition evidence, preferably counters or a fixed trace record
   drained outside interrupt context, for:
   - NCM open and notification/RX URB submission;
   - notification completion status/length and carrier transition;
   - bulk-IN completion status, residual, and actual length;
   - callback publication and network-worker admission;
   - NTH16 sequence/block metadata, parser result, queued frame count, protocol
     delivery, and RX rearm result; and
   - bulk-OUT submission/completion for the corresponding ARP/DHCP request.
   Do not dump packet payloads, unbounded descriptors, or one line per ordinary
   packet. Do not call heavyweight diagnostics while holding xHCI/NCM locks.
5. Extend focused production-source fixtures only where needed for the bounded
   p001 evidence handoff: callback-to-worker stage counters, DHCP zero-source
   transition/rollback, and diagnostic strings. p020 owns sequence,
   malformed-budget, and packet-filter fixtures. Notification reassembly,
   xHCI IRQ redesign, asynchronous TX statistics, and ECM remain excluded.
6. Complete the focused, sanitizer/analyzer, regression, `make -j16`, and
   bounded `qemu-system-x86_64` gates, then produce exactly one candidate image
   with an exact path and hash. Do not use `make check` or `.internal/` rules.
7. Optionally run a libusb-backed QEMU USB-host passthrough cell only if
   `0bda:8156` is host-visible and safely claimable. Use a disposable image;
   treat unavailable passthrough as nonblocking and never substitute it for the
   Latitude check.
8. Request one combined physical action. Start from a clean boot of the
   candidate image, record link/carrier and counters, use an explicit
   `net up ue0`, directly connected fixed-address peer and one bounded ARP/ping,
   then run `net dhcp ue0` from the clean or transactionally cleared state and
   one post-lease ping. Preserve the first failed stage and elapsed result.
9. If the combined check passes, synchronize the achieved p001 milestone. If it
   fails, do not add an unapproved repair or ask for a second boot: mark the
   current item honestly, retain the discriminator, and queue `ws004-p019` CDC
   ECM/QEMU next.

## Automatic q029 result

The safe automatic slice completed on 2026-08-29:

- `dhcpc` waits for carrier inside the same total timeout, clears the previous
  address before DISCOVER, and transactionally restores flags, address, mask,
  broadcast, and any pre-existing default route after failure;
- the receive loops recheck the absolute deadline, so a stream of irrelevant
  or invalid packets cannot extend DHCP indefinitely;
- offer, ACK, interface-configuration, route, and resolver failures report the
  failed stage; `ENETDOWN` and `ETIMEDOUT` have distinct libc strings; and the
  production DHCP builder is covered for zero `ciaddr`;
- resolver replacement keeps the prior file and the originating error across
  write, sync, close, and rename failure, and always retires its temporary
  descriptor and file;
- the focused userland network recovery fixtures passed ordinary,
  ASan/UBSan, and analyzer modes; and
- the production amd64 image was built with `make -j16`. A disposable USB-root
  copy reached `login:` in QEMU within the 30-second cell with no USB Storage
  error.

Candidate image:
[`build/amd64/hdd-image.img`](../../../build/amd64/hdd-image.img), 135266304
bytes, SHA-256
`267315c0c002def1400c8f9fb2d97c1166b69af9ee557550cde4079b8033c719`.

The optional libusb USB-host cell was unavailable because no host USB device
was visible. That is nonblocking. The single physical Latitude/RTL8156 action
is still pending, so this Phase remains in progress. No physical carrier,
static/DHCP transfer, or broader kernel stage-counter/trace result is claimed.

## Completion conditions

- Opening `ue0` with a connected cable reaches a truthful online/carrier state,
  or reports a bounded actionable link error without freezing.
- The first inbound notification and first bulk-IN NTB both return through xHCI
  completion and the deferred network worker; no CPU, console, or scheduler
  freezes.
- Static addressing permits ARP and a bidirectional peer ping.
- `net dhcp ue0` obtains a lease through `dhcpc`, installs the expected dynamic
  route and resolver data, and a post-lease peer ping succeeds.
- Existing direct `/sbin/ifconfig` recovery, fd-3 readiness, NCM/xHCI ownership,
  USB Storage, builds, and QEMU USB-root regressions remain passing.
- A pre-existing static address is cleared before DHCP DISCOVER, failure
  restores it transactionally, and automatic packet evidence shows IPv4 source
  zero with BOOTP `ciaddr` zero.
- Immediate `ENETDOWN` and elapsed `ETIMEDOUT` failures have distinct useful
  diagnostics rather than `Unknown error`.
- One final combined Latitude acceptance covers link, static peer ping, DHCP,
  and post-lease ping. Broad external networking, reconnect, and repeated-run
  reliability remain later NET-T10/NET-T40 work.

## Reconsideration boundary

Stop after the one authorized physical check rather than guessing if it still
fails, evidence shows that RTL8156 requires undocumented vendor initialization,
the failure is a general xHCI hardware-completion defect affecting other
classes, notification reassembly or a new UAPI is required, or the automatic
path cannot expose enough bounded state safely. Record the last completed
milestone and exact error/status; `ws004-p019` is the next Queue item after such
a failure, not implementation scope inside q029.
