# Queue: Archer T3U Plus final reopen acceptance

Last updated: 2026-09-06

QID: `q082`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q081](queue-q081.md)

Authorization: the user's 2026-09-06 instruction explicitly requests saving
the plan and executing both-band operation on the exact adapter. This finite
continuation closes the remaining lifecycle observation within that scope;
the purpose was presented before execution. No further approval is needed.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws004-p046](ws004-hardware/phase046-archer-t3u-plus-driver/phase.md) | uncleared | The single launch found EBUSY on the first data-to-disconnect transition; q083 localizes normal retirement before final reopen acceptance |

## Finite budget and procedure

Review after 30 active minutes. At most one exact-device amd64 QEMU launch,
capped at 20 minutes, with the existing bounded boot/network waits. Retain
the already verified production and overlay hashes; no driver change or
build is planned. The corrected harness adds final enable/fresh scan/down
after the 5-GHz data/disconnect/down sequence, and makes guest-image cleanup
independent of observer/restoration exceptions. Verify the fixture with
focused fault injection before the launch.

Use `qemu-system-x86_64`, the exact `2357:0138` on its own emulated xHCI and
a disposable guest image on the authorized host. Preserve the RTL8156 wired
management link and AX211, then restore the target to unbound state. Keep
the pinned firmware, calibrated 20-MHz W52 scope and all existing controls.
If the single launch fails, record its stage and concrete new evidence;
do not repeat it unchanged or claim completion from q081 alone.

The supplied credentials remain only in the explicitly authorized private
runtime file and disposable guest store. Read no unrelated `.internal/`
material, keep supplied values out of plans/source/logs and the key out of
process arguments, and delete the guest store after QEMU exits. No commits
or aggregate `make check` runs are authorized.

Record [q082 evidence](ws004-hardware/tests/q082-results.md), verify final
host/artifact restoration and synchronize P/W/M before closing the Queue.

## Result

The single launch completed 2.4-GHz authorization, DHCP, ping and HTTP checksum,
then normal managed disconnect returned EBUSY. Retained status still showed
connected/authorized with error zero, while L3 had been removed. Thus the exact
failed cleanup stage is not established by the generic networkd diagnostic.
The final 5-GHz and reopen steps were not attempted. All host/input restoration
checks pass, and the credential-bearing guest was deleted. Q083 carries this
new normal-retirement finding within the same authorized product objective.
