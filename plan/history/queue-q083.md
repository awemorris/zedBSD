# Queue: Archer T3U Plus normal retirement completion

Last updated: 2026-09-06

QID: `q083`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](../master.md)

Previous Queue: [q082](queue-q082.md)

Authorization: this is a finite continuation of the user's explicit request
to save and execute the dual-band implementation plan. Q082 discovered a
normal data-to-disconnect EBUSY before its final reopen check. The finding and
continued correction were presented to the user; the existing authorization
covers this work without a new approval step.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws004-p046](../ws004/phase046/phase.md) | completed | Exact SuperSpeed device passes three complete cycles on both bands and final fresh reopen/down; bounded disconnect EBUSY handling and all focused/build gates pass |

## Finite scope and budget

Review after 60 active minutes. At most three exact-device QEMU launches,
each at most 20 minutes, using the existing boot/network bounds. First trace
networkd's retirement stages, wifi's disconnect primitive, the common station
retirement intent and the RTL8822BU checked inverse operations. EBUSY is an
intentional transient result while admitted TX or hardware queues drain; do
not remove key/queue barriers or retry unrelated permanent errors. The current
fully-connected/error-zero terminal snapshot does not alone localize a driver
inverse failure. Add bounded nonsecret stage diagnostics if necessary.
The diagnostic fixture may perform at most three sequential dual-band cycles
inside one 20-minute guest to observe the intermittent retirement boundary;
it stops at the first failure and never retries a failed command automatically.

Implement only the source-localized retirement correction, with focused
regressions for transient progress, permanent/bounded failure, preserved
ownership and terminal state. A correction launch requires a concrete change
or a newly defined diagnostic question. Complete relevant host gates and
serialized amd64/PCAT/PC98 `make -j16` builds with explicit ZEDBSD_CONFIG before
freezing/hashing each modified runtime image. No builds run during QEMU.

Acceptance uses the exact SuperSpeed 2357:0138 on the authorized host with a
separate QEMU xHCI and no emulated NIC. Require ordinary two-band WPA2/CCMP,
DHCP, ping, HTTP checksum, disconnect/down, reopen between bands and a fresh
completed scan after final 5-GHz down followed by complete terminal down.
Retain the existing 20-MHz W52 scope and unchanged pinned firmware. Preserve
the RTL8156 management route and AX211, restore the target to unbound state,
verify all frozen hashes and remove the credential-bearing guest after exit.

Supplied values remain solely in the explicitly authorized private runtime
file and disposable guest store; do not read unrelated .internal material or
write supplied values to plans/source/logs or the key to process arguments.
No commits or aggregate make check runs. Record q083 evidence and synchronize
P/W/M; close uncleared with a concrete new resume condition if the budget ends.

## Result

Completed in one of the three available launches. Both bands pass three cycles
of ordinary authorization, DHCP, ping, HTTP checksum and disconnect/down.
Final post-5-GHz reopen advances completed scan generation 33 to 38, then
returns to complete down with no authorization/key state. Focused USB,
networkd-retirement and wifi-command gates, independent reviews and all three
configured x86 builds pass. Frozen image identities and host restoration are
verified; remote trial directories and secret guest images are removed.
The runtime did not reproduce EBUSY; deterministic cases verify its bounded
handling. Exact original failure attribution remains an evidence limit, not
an assertion that every hardware failure source has been audited.

See [q083 results](../ws004/tests/q083-results.md) for hashes, observations
and the SuperSpeed/W52 acceptance limits. P046/W/M are synchronized.
