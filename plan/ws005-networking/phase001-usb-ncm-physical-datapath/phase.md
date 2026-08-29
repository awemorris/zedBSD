# WS005 Phase 001: USB NCM physical data-path isolation

Last updated: 2026-08-29

Phase ID: `ws005-p001`

Status: planned; waiting for `ws004-p019` QEMU control path

Parent: [WS005 networking and WPA](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Make the generic CDC NCM `ue0` path on the Dell Latitude 5320 and the physical
RTL8156 complete its first carrier, transmit, and receive transactions without
freezing. Prove a static peer ping first, then prove `net dhcp ue0` through the
existing `networkd` and `dhcpc` path.

This Phase instantiates the first bounded slice of NET-01 and NET-10. It starts
from a class driver which is already selected and bound; it does not reopen USB
configuration matching or add a Realtek VID-specific frontend without new
evidence.

Before another physical-debug cycle, `ws004-p019` must establish an independent
CDC ECM path against QEMU `usb-net`. A passing ECM path is the control for the
common xHCI, USB, `net_device`, ARP/IP/UDP, `networkd`, and `dhcpc` layers. It
does not prove NCM framing, but it prevents those common layers from remaining
uncontrolled variables in the RTL8156 diagnosis.

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
- `ws004-p019`: independent CDC ECM plus QEMU end-to-end common-path control.

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
   delivered frames rather than a malformed completion. One nonzero initial
   sequence, lost NTB, or rejected NTB can therefore cause permanent rejection
   and a completion/rearm live loop. This is the strongest static candidate and
   overlaps the known `ws004-p017` recovery topic; it is not yet proven by
   physical evidence.
6. RTL8156-family functions may repeat connection notifications and split a
   speed-change header and payload across transfers. The current notification
   parser accepts only exact 8-byte connection or exact 16-byte speed records.
   A fixed trace must establish the actual device behavior before adding any
   bounded reassembly or duplicate suppression.
7. The NCM attach path programs `SET_ETHERNET_PACKET_FILTER` before selecting
   the active data alternate. A function is allowed to lose interface-local
   state across `SET_INTERFACE`; reapplying the filter after the final alternate
   transition is a focused ordering candidate.
8. Two userland issues can hide or confound the device failure: `networkd`
   collapses most child failures to `EIO`, while missing `strerror()` entries
   obscure `ENETDOWN`/`ETIMEDOUT`; and `dhcpc` does not clear an old static
   address before a zero-address DHCP DISCOVER. These require focused tests and
   fixes even if they are not the freeze's root cause.

## Planned implementation

1. Complete `ws004-p019` first. Use its QEMU ECM result to close generic
   xHCI/USB/network-stack and DHCP orchestration defects before requesting any
   further RTL8156 action from the user.
2. Reproduce the smallest physical static path, with an explicit `net up ue0`, a
   directly connected peer, fixed IPv4 addresses, one ARP exchange, and a
   bounded single ping. This removes `networkd` child and DHCP parsing from the
   first fault boundary.
3. Add bounded transition evidence, preferably counters or a fixed trace record
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
4. Add focused production-source fixtures for notification-before/after open,
   carrier publication, simultaneous notification and bulk completions,
   repeated and split notification records, callback-to-worker handoff, a
   nonzero initial sequence, loss/malformed then valid NTBs, bounded malformed
   completion accounting, packet-filter ordering, and TX completion. Retain
   sanitizer/analyzer and existing xHCI/NCM/net-device lifetime gates.
5. Fix the first proven stop point at its owning layer. Keep class-neutral fixes
   in USB/xHCI or `net_device`; keep NCM wire/runtime policy in the NCM module.
   Do not introduce a Realtek quirk merely because the test device is RTL8156.
   A valid sequence-resynchronization change must continue to reject malformed
   datagrams and consume bounded worker budget; it must not weaken the strict
   transactional parser.
6. Once static ARP/ICMP succeeds, run `net dhcp ue0` and preserve the child
   stage/error. Correct any independently proven orchestration or socket issue,
   then verify lease address, route, DNS output, and one post-lease ping.
7. Complete automatic gates before requesting one combined physical acceptance
   action. Repeated reconnect/reliability testing is deferred rather than
   blocking every intermediate fix on human work.

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
- The independent `HW-T22`/`NET-T42` QEMU ECM baseline remains passing after
  every shared-layer correction.
- One final combined Latitude acceptance covers link, static peer ping, DHCP,
  and post-lease ping. Broad external networking, reconnect, and repeated-run
  reliability remain later NET-T10/NET-T40 work.

## Reconsideration boundary

Stop and split the Phase rather than guessing if evidence shows that RTL8156
requires undocumented vendor initialization, the failure is a general xHCI
hardware-completion defect affecting other classes, sequence recovery requires
an unresolved p017 policy decision, or the network stack cannot expose enough
bounded state without a new UAPI. Record the last completed milestone and the
exact error/status before moving ownership.
