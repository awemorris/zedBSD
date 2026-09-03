# Queue: RTL8822BU passthrough regression reproduction

Last updated: 2026-09-03

QID: `q068`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-09-03 the user asked to prioritize the attached
RTL8822BU over AX211, use `awe@10.0.10.25` for QEMU USB passthrough, and make
the first Queue reproduce the failure only.

Timebox: none; one bounded reproduction attempt, with at most one fresh-scan
replacement attempt as defined by the Phase.

Parent: [master plan](master.md)

Previous Queue: [q067](queue-q067.md)

## Purpose

Reproduce the current exact-RTL8822BU `/sbin/wifi connect` `ENOENT` failure
under controlled QEMU xHCI USB passthrough and determine whether it occurs in
common BSS selection or after the RTL driver admits the connection.  Produce
evidence suitable for planning the smallest quality correction; do not fix it
in this Queue.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p040` | [RTL8822BU passthrough regression reproduction](ws004-hardware/phase040-rtl8822bu-passthrough-reproduction/phase.md) | completed | Exact passthrough reproduced immediate `ENOENT`: the controlled 5-GHz target was absent from the completed channel-1 snapshot, so common selection rejected it before an RTL connection generation |

## Fixed boundaries

- Exact target is unbound USB `2357:012e` on `10.0.10.25`.
- Do not pass through the RTL8156 host-network device or assign AX211 to VFIO.
- Use a disposable current amd64 image and keep the SSH route available.
- Credentials and network identities remain runtime-only and absent from
  plans, tracked fixtures, retained logs, and process listings.
- No driver, common WLAN, command, firmware, policy, or diagnostic source
  change is authorized by q068.
- Do not run aggregate `make check`; the only build gate is the current amd64
  image with `make -j16`.

## Stop and completion rule

Q068 finishes after the single Phase is processed.  Mark p040 `completed` only
when the reported error is reproduced and classified.  Mark it `uncleared` if
the exact bounded environment does not reproduce, recording the immutable
candidate and next discriminator.  In either case, return to planning before
any correction or broader RTL8822BU quality work.

## Result

Q068 is finished. The exact `2357:012e` device attached as `wlan0` through its
own emulated xHCI controller, and generation 1 completed with two redacted
channel-1 BSS records. The controlled 5-GHz target was absent. Its exact direct
connect returned `ENOENT` synchronously; immediate status remained idle with
zero retries and zero terminal error. Production source confirms the first
boundary is common BSS selection and that the RTL driver currently exposes
only channels 1--11. No source correction was made.

The immutable image, environment, redacted observation, host restoration, and
raw-capture disposal are recorded in
[q068 evidence](ws004-hardware/tests/q068-rtl8822bu-passthrough-evidence.md).
The smallest resulting quality correction is planned, but deliberately not
queued, as [`ws004-p041`](ws004-hardware/phase041-rtl8822bu-5ghz-quality/phase.md).
