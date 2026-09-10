# Queue q229: USB synchronous wait scheduling

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q228](queue-q228.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Replace pending URB busy-spin with cooperative wait progress; targeted host completion/deadline checks; three builds and High-Speed mounted-media acceptance |

Result: pending URB waits now yield. Actual-source normal/sanitized checks and all
three builds pass; two complete High-Speed mounted-medium scenarios pass beyond
the q228 stall. Full p029 remains uncleared for retained-dirty/owner acceptance.
