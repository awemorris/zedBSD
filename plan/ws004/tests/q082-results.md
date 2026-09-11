# q082 results — P046 final-down reopen verification

Status: `finished / uncleared`

Queue: [active Queue Book](../../queue.md)  
Phase: [WS004-P046](../phase046/phase.md)  
Baseline: [q081 results](q081-results.md), Cell 4

## Scope and remaining acceptance condition

q081 Cell 4 passed normal 2.4-GHz and W52 5-GHz connection, DHCP, ping,
HTTP payload verification, disconnect, and administrative down. It also
reopened the device between the two band checks. The console ends after the
final 5-GHz down status, so it does not establish P046's remaining literal
condition: reopen and scan **after that final down**, then return to down.

q082 closes this specific evidence gap with one QEMU USB-passthrough launch
bounded to 20 minutes. It reuses the frozen q081 Cell 4 production/runtime
images. There is no driver change or image rebuild in this Queue.
Harness work is limited to the final enable/scan/disable/down checks and
credential/overlay deletion that still runs when another cleanup step raises
an exception.

## Pinned inputs

- Exact observed device: `2357:0138`, host USB path `4-4`, speed `5000`;
  guest reports USB SS with 1024-byte bulk packets.
- Guest board observation: cut `3`, RFE `3`, country `ffff`, channel plan `a6`,
  `rf-board=01`.
- Production image SHA-256:
  `12c7030e255394451fa51eff9ec152ccc9d7ff0eeef27853096450ce94b47890`.
- Fixture image SHA-256:
  `83efe8200af3f176f1fc15baa509e3522cea289c085d395b720abb22473a7187`.
- Runtime image SHA-256 (also the Cell 4 result's `input_sha256`):
  `f9d74fbdcfd2860571358739a1a7bd239ffa5359e0c534b5803cdbd6145c46b7`.
- Identity source: [identity4.json](../temp/q081-runtime/identity4.json);
  payload LBA `133120`. Reuse these existing images without rebuilding.
- q082 runner SHA-256:
  `16af7f1018b3aee7b9a30bab54f58ccbff6821e582218da736d74529984c69d9`.
- Descriptor observer SHA-256:
  `f00505128a591cadf760ba621263a8b7a4b57b51ef977bc2c88ca1c590143700`.
  The runtime source copy matches the existing Cell 4 input hash at launch.

## Verified q081 Cell 4 baseline

Evidence: [result.json](../temp/q081-runtime/cell4/result.json) and
[console-redacted.log](../temp/q081-runtime/cell4/console-redacted.log).

| Check | 2.4 GHz | 5 GHz / W52 |
| --- | --- | --- |
| Connected channel | 1 / 2412 MHz | 44 / 5220 MHz |
| Authentication, association, installed key, authorized port | yes; error 0 | yes; error 0 |
| Negotiated baseline | WPA2 / CCMP / PSK | WPA2 / CCMP / PSK |
| DHCP address and default route | present on `wlan0` | present on `wlan0` |
| Ping | 3 received / 3 sent | 3 received / 3 sent |
| HTTP fetch | exit 0 | exit 0 |
| Payload length | 4480 bytes | 4480 bytes |
| POSIX `cksum` | `4004478192 4480` | `4004478192 4480` |
| Disconnect and disable | exit 0 | exit 0 |
| Terminal status | down; no auth/association/key/authorization; error 0 | down; no auth/association/key/authorization; error 0 |

The initial scan completed with 11 entries. After the 2.4-GHz down, enable
started scan generation 8 and a fresh completed snapshot contained 11 entries
before the 5-GHz connection. A previously cached BSS list displayed while
`scan state=1` is not evidence of the new scan completing.

The host descriptor observer recorded 114 C2H first records and 1328 ordinary
frame first records. These are first-record classifications per observed USB
completion, not complete aggregate packet totals. The observer stopped without
an error. Its maximum observed control-transfer duration was 268 microseconds;
this observation alone does not establish a worst-case transport deadline.

`result.json` records `status=passed`, target present and restored to its
unbound state, unchanged host routes, and unchanged source input. It does not
record a post-final-down reopen or prove exception-path private-file deletion.
Those are q082 closure checks.

## q082 execution and terminal evidence

Launch count: `1 / 1`. Runtime result: **failed at 2.4-GHz disconnect**.

Before launch, focused harness fault injection and independent review pass.
Observer, route and digest exceptions cannot skip credential-image removal
after QEMU has exited; unknown/live process ownership is an explicit cleanup
failure. Original runtime errors are retained, and cleanup messages contain
exception types only. The final scan accepts only a generation newer than
the pre-enable snapshot, ignoring cached completed/cancelled/failed states.
Fresh failure and stale-only timeout remain failures. Terminal down requires
all connection/key/authorization flags cleared and error zero. Source syntax
and whitespace checks pass. No production rebuild or change is made.

The retained result must show:

1. The unchanged Cell 4 image identities and the q082 harness identity.
2. Both band transactions reaching their checked down states, followed by a
   final enable that produces a newly completed scan generation. Cached BSS
   entries from an earlier scan do not satisfy this step.
3. Final disable and status showing administrative down with authentication,
   association, key installation, and authorization all cleared, and error 0.
4. QEMU and the observer stopped; exact USB binding and host routes restored;
   source images unchanged; disposable credential/overlay inputs deleted.

The single launch ends on completion, a concrete failure, or its 20-minute
bound. Record any failure and the resulting restoration state before assigning
the terminal Queue/Phase result. P046 completion remains pending until the
post-final-down reopen evidence and cleanup checks are retained.

## Terminal result and continuation

The unchanged input again passed 2.4-GHz channel-1 authorization, DHCP,
three ICMP replies, 4480-byte HTTP transfer and checksum `4004478192`.
The subsequent `net wifi disconnect` returned EBUSY (17), reported by networkd
as degraded managed cleanup. Status remained connected/authenticated/
associated/key/authorized with error zero; ifconfig retained UP/RUNNING
but had no IPv4 address. No control/scan/recovery diagnostic appeared. The generic
message does not distinguish a disconnect-child failure from later L3 cleanup,
so the exact origin is not yet claimed. No 5-GHz/final-reopen check ran.

All restoration checks pass; QEMU/observer exited, target is present/unbound,
routes and source input are unchanged, and guest.img is deleted. Q083 adds a
bounded source/stage diagnosis and correction of normal retirement before
repeating the complete acceptance contract. Q081's dual-band communication
success remains valid, but P046 is not complete while this lifecycle failure
is unresolved.
