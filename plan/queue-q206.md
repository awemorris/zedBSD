# Queue q206: high-speed pending-read disconnect

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q205](queue-q205.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Trace-proven high-speed pending-read disconnect, same-device replug/readback and halt |

Use q205's physical attached=false/true path on EHCI. No SCSI backend replacement
claim. Preserve failures; do not repeat completed SS acceptance without changes.

Result: EHCI native pending-read removal exposed permanent fallback quarantine;
checked endpoint/zero-owner retirement fixes it. Host barrier checks, native
high-speed disconnect/replug/readback/halt and three builds pass. Full phase
retains remaining replacement-media and uncertain-write acceptance.
