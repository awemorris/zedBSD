# Queue: RTL8822BU 2.4-GHz connect reproduction

Last updated: 2026-09-03

QID: `q069`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-09-03 the user clarified that the same `ENOENT` occurred
with the controlled 2.4-GHz SSID and explicitly requested one QEMU passthrough
attempt against that SSID.

Timebox: one fresh scan and exactly one valid direct-connect attempt.

Parent: [master plan](master.md)

Previous Queue: [q068](queue-q068.md)

## Purpose

Test the missing q068 case: direct connection to the exact controlled 2.4-GHz
SSID after it appears in a completed RTL8822BU scan. Classify the one result
before revising the separately planned quality correction.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p042` | [RTL8822BU 2.4-GHz connect reproduction](ws004-hardware/phase042-rtl8822bu-24ghz-connect-reproduction/phase.md) | completed | The single exact channel-1 attempt reached generation 2, authorized connection, and zero-error status; the user's `ENOENT` did not reproduce |

## Fixed boundaries

- Exact target is unbound USB `2357:012e` on `10.0.10.25`.
- Do not pass through the RTL8156 host-network device or assign AX211 to VFIO.
- Reuse a disposable copy of q068's immutable amd64 image and keep the SSH
  route available.
- Credentials and network identities remain runtime-only and absent from
  plans, tracked fixtures, retained logs, and process listings.
- No driver, common WLAN, command, firmware, policy, or diagnostic source
  change is authorized by q069.
- Do not run aggregate `make check`; the only build gate is the current amd64
  image with `make -j16`.

## Stop and completion rule

Q069 finishes after the one valid 2.4-GHz direct-connect attempt is processed.
Record success, admitted driver failure, or pre-admission rejection honestly,
then revise p041 from that evidence. Return to planning before any correction.

## Result

Q069 is finished. The exact snapshot-visible 2.4-GHz target connected on the
only authorized attempt. Scan generation 1 contained a supported channel-1
WPA2/CCMP/PSK record; connection generation 2 reached authenticated,
associated, keyed, and authorized state with zero retries and zero terminal
error. No source change or retry was made.

The result and restored host boundary are retained in
[q069 evidence](ws004-hardware/tests/q069-rtl8822bu-24ghz-evidence.md). The
user-observed 2.4-GHz `ENOENT` remains unlocalized and must not receive a
speculative correction from this passing single run.
