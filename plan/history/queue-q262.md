# Queue q262: current xHCI reservation regression

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution / obsolete test repair
Timebox: 45 active minutes
Previous: [q261](queue-q261.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](../ws025/phase030/phase.md) | uncleared | Current xHCI reservation/SG planner generation fixture ordinary/sanitizer |

No production semantics or HAL API changes to satisfy old tests.

Result: selected xHCI ordinary/sanitizer fixture passes reservation/SG/generation
checks. High address encoding is controlled, not native DMA. Remaining native/WLAN
conditions retained; no code changes, all jobs terminal.
