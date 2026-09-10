# Queue q232: borrowed-frame kernel mappings

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q231](queue-q231.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Explicit borrowed RAM vmap ownership, readonly/write permissions, rollback/retirement tests, three supported builds |

Caller retains physical pins and content ownership until release completes.
This implements the mapping foundation; syscall enablement requires the
remaining VM content lease and publication contracts and measured benefit.

Result: borrowed-frame implementation and focused verification complete;
18,060 ordinary/sanitized checks and all three builds pass. p028 remains
uncleared for content lease, syscall integration and measurement; see results.
