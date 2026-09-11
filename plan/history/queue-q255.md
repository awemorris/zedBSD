# Queue q255: native xHCI IRQ measurement

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution
Timebox: 60 active minutes
Previous: [q254](queue-q254.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](../ws025/phase030/phase.md) | uncleared | Private IMOD 4000 build, register readback, storage/HID overlap and native IRQ counter rates |

No default tuning or pending HAL interface changes. Preserve ordinary artifacts.

Result: private build and full 4000 USB/HID campaign pass, actual register 4000.
64 samples; 1181 callbacks/events over 16.5 s. Other intervals/topologies remain;
phase uncleared. Production source/config integrity passes; all jobs terminal.
