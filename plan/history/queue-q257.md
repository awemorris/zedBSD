# Queue q257: USB2 xHCI IMOD topology

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution
Timebox: 60 active minutes
Previous: [q256](queue-q256.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](../ws025/phase030/phase.md) | uncleared | Private USB2-only xHCI option and speed oracle; IMOD 4000 USB/HID/storage/IRQ acceptance |

Only test orchestration changes; production default remains 4000.

Result: USB2 root speed ID 3, IMOD 4000 readback, full native USB/HID campaign
and 64 storage samples pass. Other USB2 intervals/WLAN/recovery remain.
