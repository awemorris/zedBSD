# Queue q260: current BOT recovery fixture

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution; user authorized obsolete test repair
Timebox: 60 active minutes
Previous: [q259](queue-q259.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](ws025-io-memory-cache/phase030-imod-measurement/phase.md) | uncleared | Repair obsolete reservation runner, focused actual BOT recovery ordinary/sanitizer gate |

Production behavior trusted; no HAL API changes. Scope does not claim native IRQ timing.

Result: obsolete runner references repaired; selected actual BOT ordinary and
sanitizer gates pass. Controlled reset/control recovery covered; native/WLAN
remaining scope retained. Production source unchanged, all jobs terminal.
