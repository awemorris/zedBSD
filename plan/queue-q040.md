# Queue: Archer identity intake and network authorization foundation

Last updated: 2026-08-31

QID: `q040`

Queue status: finished

Queue finished: **Yes**

Authorization: after q039's automatic repairs completed, the user explicitly
requested continuous execution of the remaining workstreams. The standing
priority order selected the first dependency-ready WLAN evidence checkpoint
and then an independent control-plane prerequisite rather than waiting on
physical label evidence.

Timebox: none. Both finite items were processed to `completed` or `uncleared`.
The human label checkpoint in the first item did not block the second item.

Parent: [master plan](master.md)

Previous Queue: [q039](queue-q039.md)

Next Queue: [q041](queue.md)

## Purpose

Capture every safely obtainable identity, descriptor, provenance, and license
fact for the exact connected Archer T3U Nano without binding a zedBSD driver.
Then implement the independently specified AF_UNIX peer-credential and network
authorization foundation required before ordinary users or desktop software
may request WLAN orchestration through `networkd`.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p026` | [Phase](ws004-hardware/phase026-archer-t3u-nano-identity-firmware/phase.md) | uncleared | Exact descriptor, independent ID mappings, firmware/license pin, package boundary, base absence, and negative inputs are recorded; only the non-inferable printed model/region/revision awaits one external observation |
| 2 | `ws005-p003` | [Phase](ws005-networking/phase003-unix-peer-credentials/phase.md) | completed | Fixed 12-byte AF_UNIX peer snapshots, transactional `root:network 0660` publication, peer-based root/non-root policy, and central root checks for direct mutating network ioctls pass focused, analyzer, sanitizer, full-build, and native PC-98 runtime gates |

## Result

`ws004-p026` retained the exact unbound `2357:012e` descriptor from the
authorized Debian host, pinned independent TP-Link/Linux mappings to
RTL8822BU, and froze the optional `rtw8822b_fw.bin` provenance/license/digest
boundary.  A USB descriptor cannot reveal the printed product region and
hardware revision, so the Phase correctly remains `uncleared` until one
serial-redacted label photograph or transcription is supplied.  No firmware
was committed and no radio action occurred.

`ws005-p003` completed.  It added the fixed-width `SO_PEERCRED` extension,
connection-time pathname/listen/socketpair identity, checked concurrent
publication and teardown, and descriptor-transfer preservation. `networkd`
now rejects missing, malformed, duplicate, or contradictory `network` group
data, publishes and verifies one `root:network 0660` inode before READY,
authenticates before parsing, and exposes only `SHOW` to admitted non-root
peers.  The common kernel network ioctl boundary denies every present or
unknown mutation to non-root before argument/backend access.

The final production qemu-pc98 cell additionally exercised a 32-cell
listener-close/connect leak campaign, datagram reconnect compatibility,
supplementary-group admission, non-root mutation rejection, unrelated-user
pathname denial, `networkd` READY, and `init: system running`.  All reusable
test entry points are listed under NET-T22.

## Deferrals and resume conditions

- `ws004-p026`: obtain only the purchased unit's printed model, region, and
  hardware revision with serial omitted.  Reconcile it with the retained
  descriptor before p027/p028 may bind the adapter.
- `ws004-p027` remains dependency-blocked by the uncleared physical identity
  checkpoint; q040 did not widen its USB match or infer a label from
  `bcdDevice`.
- The independent `ws005-p005` profile store is eligible after p003 and moves
  into q041 without waiting for radio hardware.

## Fixed boundaries honored

- No `.internal/` test material or aggregate `make check` target was used.
- The adapter remained unbound and its serial was redacted.
- No Realtek blob entered the zlib-licensed base tree and no runtime/build
  download was introduced.
- No world-writable or second daemon socket, setuid client, payload-supplied
  identity, or non-root direct mutation path was added.
- The Phase commit is local.  The previously rejected push was not retried.

