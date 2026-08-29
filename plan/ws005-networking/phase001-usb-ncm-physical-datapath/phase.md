# WS005 Phase 001: USB NCM physical data-path isolation

Last updated: 2026-08-29

Phase ID: `ws005-p001`

Status: in progress (`q029`); automatic and real-device QEMU-passthrough paths
pass, final Latitude-native check pending

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

The original decision rule put `ws004-p019` after a failed physical check. The
first check did fail, but the user then explicitly authorized evidence-led
remote passthrough debugging. That debugging proved the physical NCM data path
and removed the need to use ECM as an immediate diagnostic detour. ECM remains
outside q029 and this Phase's authorized implementation slice.

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

## First q029 hardware result and evidence-led repair

The first q029 Latitude action reached `ue0` but stopped at the carrier
boundary:

```text
net: 5 dhcpc: ue0: carrier: Connection timed out
```

The user then authorized remote debugging on `awe@10.0.30.4`, where the
`0bda:8156` adapter was not carrying the host's SSH route. A disposable zedBSD
guest booted from IDE and received that physical adapter through QEMU's
libusb-backed `usb-host`; xHCI therefore carried only the Ethernet device.
This reproduced `ue0 unconfigured offline` with the pre-repair image and
captured the interrupt endpoint without adding packet logging to the kernel.

The USB capture proved that endpoint `0x83` was completing successfully and
repeated these valid notifications:

```text
a1 2a 0000 0100 0800 00f90295 00f90295
a1 00 0100 0100 0000
```

They are respectively `SPEED_CHANGE` at 2.5 Gbit/s in both directions and
`NETWORK_CONNECTION` with `wValue=1`. Both use `wIndex=1`, the validated NCM
data interface. The driver accepted only `wIndex=0`, the communication
interface, and silently discarded both records. Length, request type,
`wValue`, and `wLength` were otherwise valid; no notification reassembly or
vendor-specific initialization was needed.

The repair accepts either member of the control/data pair already validated by
the NCM binding and still rejects every unrelated interface number. It does
not match on the Realtek VID:PID or weaken the notification-specific shape
checks. The production-source fixture covers connection and speed records with
data-interface `wIndex`, retains control-interface coverage, and rejects an
unrelated interface.

## Dependencies

- `ws002-p020`: `net` -> `networkd` -> `dhcpc` baseline and direct-ifconfig
  recovery path.
- `ws004-p012`: removable `net_device` and deferred polling lifetime.
- `ws004-p014`: integrated CDC NCM wire/driver and URB lifecycle.
- `ws004-p018`: physical Union-only NCM selection, bind, and `ue0` publication.
- `ws004-p020`: deterministic valid-sequence/resynchronization policy, bounded
  completion work, and packet-filter programming on open after the active
  alternate. Complete in q029.

`ws004-p019` remains an independent successor candidate, not a q029
dependency. The more specific native periodic-context follow-up discovered by
this Phase is planned as `ws004-p021` and is also outside q029.

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
   bounded `qemu-system-x86_64` gates, then produce a candidate image with an
   exact path and hash. If retained physical evidence identifies a bounded
   repair, supersede it once and retain both hashes. Do not use `make check` or
   `.internal/` rules.
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
   fails, do not add an unapproved repair: mark the current item honestly,
   retain the discriminator, and return the evidence to a newly approved
   Queue. The later user-authorized remote discriminator is recorded below.

## Automatic and remote-passthrough q029 result

The safe automatic slice completed on 2026-08-29:

- `dhcpc` waits for carrier inside the same total timeout, clears the previous
  address, snapshots and removes the interface's old default route before
  DISCOVER, and transactionally restores flags, address, mask, broadcast, and
  the exact pre-existing default route after failure;
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
- the repaired NCM production-source fixture passed 1540 checks in ordinary
  and ASan/UBSan modes plus its analyzer; NCM wire, USB binding (971 checks),
  concurrent xHCI/USB function (1404 checks), and removable-network-device
  regressions passed; and
- the production amd64 image was rebuilt with `make -j16`. A disposable
  xHCI/USB-Storage copy reached `login:` in QEMU within the 30-second cell with
  no USB Storage error.

Candidate image:
[`build/amd64/hdd-image.img`](../../../build/amd64/hdd-image.img), 135266304
bytes, SHA-256
`267315c0c002def1400c8f9fb2d97c1166b69af9ee557550cde4079b8033c719`.

That earlier candidate was superseded after the capture-led notification
repair. The first repaired candidate was the same path and size, SHA-256
`982201f8f5f2b00632c3c3e7d9437504fe6dcc110b49419ca455e0b7df0cb7bf`.

That image passed clean DHCP with the real device. A stronger static-to-DHCP
run then exposed a second deterministic transaction defect: USB capture
contained no DHCP client frame because `dhcpc` cleared the static address while
leaving its default route installed until after ACK. The failed run restored
the exact static address and route. The correction moves default-route
snapshot/removal before socket creation and DISCOVER; focused fixtures now
require the route to be absent at every DHCP send and cover snapshot, delete,
send, offer-timeout, configuration, resolver, routerless-success, and
router-success boundaries.

The final current candidate is the same path and size, SHA-256
`34341960f871335f9ff40177664d1d0da017ce1cd3497aff0ad45658adb06e46`.
It then passed the real-device USB-host cell end to end:

- `net up ue0` produced `ue0 unconfigured online`;
- static `10.0.30.2/16` exchanged ICMP with host `10.0.30.4` before the
  transition;
- USB capture retained both zero-source client broadcasts
  `0.0.0.0:68 -> 255.255.255.255:67` and the DHCP-server responses;
- `net dhcp ue0` retained/acquired `10.0.30.2/16` and replaced the static
  default with dynamic gateway `10.0.0.1`;
- `ifconfig ue0` reported `UP,RUNNING` with RX and TX traffic; and
- after DHCP, two pings each to `10.0.0.1` and host `10.0.30.4` returned with
  zero loss.

This proves the physical RTL8156 NCM notification, DHCP, NTB TX/RX, ARP, IPv4,
and ICMP path through the QEMU xHCI model. It does not substitute for the
Latitude's native xHCI controller, so the Phase remains in progress until one
combined Dell check uses the current image.

A separate static audit found that the native xHCI endpoint-context builder
does not encode SuperSpeed periodic Max ESIT Payload. QEMU tolerates that
independent specification defect. Its bounded correction is planned as
`ws004-p021` and is not silently added to q029 implementation scope.

The final review also retained two nonblocking multi-process/multi-interface
follow-ups rather than broadening q029: IPv4 limited broadcast must ignore a
different interface's global default gateway when `SO_BINDTODEVICE` selects the
DHCP interface, and direct concurrent `dhcpc` invocations need route-transaction
ownership or serialization before they can promise not to overwrite a route
changed by another process. The q029 acceptance topology has one routed
Ethernet interface, so neither changes its recorded result.

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

Stop after the final repaired-image Latitude check rather than guessing if it
still fails. A carrier-only failure now points first to the independently
planned `ws004-p021` SuperSpeed periodic endpoint-context correction because
the same physical adapter and NCM stack pass through QEMU. Keep
`ws004-p019` as an independent ECM baseline, not the automatic next action
after evidence has already proven physical NCM interoperability. Any remaining
failure must retain its first stage and be returned through a newly approved
Queue rather than silently expanding q029.
