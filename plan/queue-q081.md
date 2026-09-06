# Queue: Archer T3U Plus TX completion and dual-band acceptance

Last updated: 2026-09-06

QID: `q081`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q080](queue-q080.md)

Authorization: on 2026-09-06 the user explicitly requested saving a plan and
then executing it, including useful 2.4-GHz and 5-GHz operation on the new
adapter. The USB explanation and initial finite scope were presented before execution.
Q080 exhausted its four-launch budget; this continuation uses the new
RX-positive/C2H-zero finding within the same authorized dual-band objective.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws004-p046](ws004-hardware/phase046-archer-t3u-plus-driver/phase.md) | uncleared | Both bands pass authentication, DHCP, ping, HTTP and down in cell 4; only the final post-5-GHz reopen remains for q082 |

## Finite budget and procedure

Review after 90 active minutes. At most four exact-device amd64 QEMU launches,
each capped at 20 minutes, with 120-second boot waits and bounded network waits.
Each correction run requires a diagnosed cause and a concrete change. Close and
record unresolved stages when exhausted; a follow-up Queue uses new evidence.

The measured profiles, speed programming and board admission are implemented.
Q080 received 78 frames but no C2H notifications before three TX timeouts.
Compare TX queue/endpoint selection, management descriptors and firmware report
setup with primary upstream sources; implement only localized corrections and
bounded nonsecret diagnostics. Retain checked TX completion and the pinned firmware. Run relevant
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

Record [q081 evidence](ws004-hardware/tests/q081-results.md), synchronize
P/W/M and close the Queue truthfully.

## Result

All four launches are consumed. Cell 4 passes both bands on the exact
SuperSpeed adapter, including data checksum and reopen between bands. The
fixture omitted reopen after the final 5-GHz stop. Q082 closes that remaining
P046 lifecycle condition with a corrected fixture and unchanged production
image; no new driver defect is inferred. Host and image restoration pass.
