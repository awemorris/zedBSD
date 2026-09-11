# Queue q202: SuperSpeed reset and reprobe

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 120 active minutes
Previous: [q201](queue-q201.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Checked SS device reset, transport reconstruction and fixed-media reprobe; host failure checks and native timeout recovery |

Keep failed writes sticky. Changed or removable media cannot inherit old BIOs;
media-generation admission remains a separate outstanding phase requirement.

Result: fixed-medium SS reset/reprobe recovery implemented and native timeout/new
read passes with reset trace and pcap. Host failure checks and all three builds
pass. Full p029 remains uncleared for media-generation and remaining acceptance.
