# Queue q261: current USB core ownership regression

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution / obsolete test repair
Timebox: 45 active minutes
Previous: [q260](queue-q260.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](../ws025/phase030/phase.md) | uncleared | Selected actual USB core reservation/recovery lifetime fixture, ordinary/sanitizer |

No pending HAL declaration changes. Record tested ownership cases without
claiming native timing or whole recovery-matrix completion.

Result: selected USB core ordinary/sanitizer fixture passes reservation rollback,
cancel failure and delayed completion ownership. No code changes needed.
Broader native/WLAN acceptance remains; all jobs terminal.
