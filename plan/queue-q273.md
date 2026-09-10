# Queue q273: output alias unmap batching

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution
Timebox: 60 active minutes
Previous: [q272](queue-q272.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Coalesce output alias unmaps; actual VM failure/ownership checks and supported builds |

Result: batching implementation, actual VM ordinary/sanitizer tests and three builds PASS. Native output acceptance pending; p028/default-off unchanged.
