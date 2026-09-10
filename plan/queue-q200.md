# Queue q200: SuperSpeed UAS recovery baseline and reset contract

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q199](queue-q199.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Reproduce SS timeout/new-read failure and resolve old-status retirement against actual USB reset ownership before implementing recovery |

Use disposable QEMU backing and the existing throttled-read cell. A failure is
baseline evidence, never an acceptance pass. Determine reset/stream reallocation,
media identity and sticky-write handling before enabling tag reuse.

Result: native SS timeout/new-read failure reproduced (test exit 1), and checked
reset/reprobe ownership contract recorded. No production implementation claim.
Full phase remains uncleared; next implement recovery and rerun this baseline.
