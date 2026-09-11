# Queue q143: xHCI IMOD functional comparison

Date: 2026-09-09
Status: finished
Authorization: User resumed autonomous Priority work after switching to Astra.
Timebox: 60 active minutes
Previous: [q142](queue-q142.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](../ws025/phase030/phase.md) | uncleared | All three actual register and storage/HID campaigns pass; write/fsync and IRQ/CPU/latency measurement cells remain |

Dependencies: q139 NVMe acceptance and q142 heap/lifecycle closure. Use the
existing IMOD build override and maintained USB/HID campaign. Keep default 4000;
emulator timing cannot establish a physical performance optimum. Record any
remaining measurement cells explicitly instead of calling partial coverage done.
