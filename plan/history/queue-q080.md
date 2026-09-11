# Queue: Archer T3U Plus USB transport and dual-band acceptance

Last updated: 2026-09-06

QID: `q080`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](../master.md)

Previous Queue: [q079](queue-q079.md)

Authorization: on 2026-09-06 the user explicitly requested saving a plan and
then executing it, including useful 2.4-GHz and 5-GHz operation on the new
adapter. The USB explanation and finite scope were presented before execution.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws004-p046](../ws004/phase046/phase.md) | uncleared | Extend exact Plus HS/SS transport and prove both supplied bands; depends on completed p045 and the existing RTL8822BU/WLAN baseline |

## Finite budget and procedure

Review after 90 active minutes. At most four exact-device amd64 QEMU launches,
each capped at 20 minutes, with 120-second boot waits and bounded network waits.
Each correction run requires a diagnosed cause and a concrete change. Close and
record unresolved stages when exhausted; a follow-up Queue uses new evidence.

Implement the measured identity profiles and private USB speed programming,
verify actual board facts, and retain the pinned firmware. Run relevant
production-linked host gates and serialized supported x86 builds using
`make -j16` with explicit `ZEDBSD_CONFIG`. Finish builds before copying and
hashing runtime artifacts. Use `qemu-system-x86_64`, a disposable image and the
exact adapter on its own emulated xHCI. Preserve the host RTL8156 management
link and AX211; restore the target to unbound state.

For each supplied band require fresh scan, WPA2/CCMP authorization, DHCP,
bounded ping/data transfer, and clean disconnect/down/reopen. Keep existing
W52 calibration and conservative power limits. Cell 1 localized the measured
`a6/ffff` board's exclusion; p046 now admits that documented hardware-enforced
ETSI plan's W52 subset while preserving unknown/contradictory-plan rejection. Broader
RF frontend, DFS and automatic USB speed switching remain outside this Queue.

The user explicitly permits storing supplied credentials in `.internal/`.
This narrowly overrides the repository prohibition for this newly supplied
runtime credential file only; no unrelated `.internal/` material is consumed.
Keep all supplied values out of plans, tracked source and retained logs.
Passphrases never enter process arguments or console output; production
networkd may pass a selected SSID to its existing child command. Do not commit
or use `make check`.

Record [q080 evidence](../ws004/tests/q080-results.md), synchronize
P/W/M and close the Queue truthfully.

## Result

All four launches were processed. Exact SuperSpeed attach, firmware start and
RF reception work, and focused gates plus all three configured x86 builds pass.
Cell 4 received 78 frames but no C2H notifications, then three missing firmware
TX reports triggered checked recovery. Both-band useful communication remains
unverified. Q080 is finished with p046 uncleared, not complete. The concrete
resume condition is to localize the TX/CCX omission using this new RX-positive
evidence; the user-authorized p046 execution continues in q081.
