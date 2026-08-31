# Queue: WLAN v1 contract closure

Last updated: 2026-09-01

QID: `q053`

Queue status: completed

Queue finished: **Yes**

Authorization: the user directed automatic continuation after q052 was
committed, synchronized with origin, and pushed. The USB-LAN residual audit
found no decision-free implementation Phase: p021 awaits one physical check
and p017 awaits a shared TX-statistics decision. Under the standing rule, q053
therefore advances the next dependency-ready networking Phase.

Timebox: none. Close the one already-frozen design Phase without attributing
source, build, QEMU, or hardware evidence to it.

Parent: [master plan](master.md)

Previous Queue: [q052](queue-q052.md)

## Purpose

Complete the formal design review for the frozen WLAN v1 control contract.
Audit the master, WS005, WS004 common-core/radio Phases, and downstream command
Phases for exact topology, ownership, limits, recovery, and supersession
agreement. Correct documentation omissions only.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws005-p002` | [Phase](ws005-networking/phase002-wlan-v1-contract/phase.md) | completed | All dependent P-books carry the frozen v1 bounds and ownership; superseded `/sbin/wpa` work remains explicitly historical |

## Dependencies and exclusions

- q040 completed authenticated AF_UNIX peer identity and p003; q051 completed
  p005's credential store.
- The v1 topology, command grammar, security boundary, bounded state machine,
  and recovery policy are already frozen and have no human design gate.
- q053 does not implement the WLAN UAPI, `/sbin/wifi`, `networkd` framing,
  orchestration, Realtek transport, WPA2, or physical radio behavior.
- p026's purchased-unit label remains required before p027/p028 implementation
  crosses a future Queue boundary.
- p017's TX statistics decision and p021's physical Latitude checkpoint are
  recorded as deferred USB-LAN boundaries, not q053 Queue items.

## Fixed review boundaries

- Preserve `net` -> `networkd` -> `ifconfig`/`wifi`/`dhcpc`; do not restore a
  resident `/sbin/wpa` or `/etc/wpa/` database.
- Preserve one 64-entry scan snapshot, 15-second scan limit, 30-second direct
  connect limit, 10-second DHCP stage, 90-second compound limit, at most four
  automatic connection attempts, and same-BSS delays 0/1/2/4/8 seconds with at
  most five failures and 30 seconds total.
- Keep kernel/common-core, chip-driver, primitive-command, daemon, public
  client, and persistent-profile ownership distinct.
- Do not use `.internal/`, aggregate `make check`, or claim an implementation
  result from this documentation-only Phase.

## Completion definition

q053 completes when every p002 design-acceptance condition is traceable in the
master, WS records, and dependent P-books; every discovered omission is fixed;
and p002 is marked complete with a concise audit result. A newly discovered
product decision leaves only that item `uncleared` and does not authorize code.

## Execution result

Q053 completed the documentation-only `ws005-p002` design closure. The frozen
control topology, ownership, privilege boundary, credential separation,
failure state, and supersession record agree across the master, WS005, WS011,
and the dependent WS004/WS005 P-books.

The audit corrected three numeric omissions without changing product policy:

- `ws004-p027` now states the 15-second total scan-generation budget and the
  30-second total direct-L2-connect budget;
- `ws004-p028` binds every scan transition and retry to the same 15-second
  total; and
- `ws004-p029` binds the complete WPA2 L2 connection, including retries, to
  the same 30-second total.

The already-owned 10-second DHCP stage, 90-second compound deadline, four
automatic attempts, 64-entry BSS snapshot, and 0/1/2/4/8-second same-BSS
recovery policy were consistent. The superseded `/sbin/wpa`, `/etc/wpa/`,
resident profile loop, and RTL8822CE-first proposal remain historical only.

No source, build, QEMU, radio, or hardware result is claimed. The USB-LAN
residuals remain deferred: `ws004-p017` needs the shared asynchronous-TX
statistics decision, and `ws004-p021` needs its one hash-pinned Latitude
observation. No further decision-free Phase is dependency-ready after q053.
