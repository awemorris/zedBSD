# Queue q221: revoked buffer discard preflight

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q220](queue-q220.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Check every revoked-medium buffer before destructive discard under existing external-user exclusion; preserve dirty buffers on busy refusal |

This does not establish cross-layer VM/mount admission by itself.

Result: discard preflight implemented; two-dirty-buffer pinned-refusal/no-write
checks and inherited host tests pass ordinary/sanitized; three builds pass.
Full phase remains uncleared. Resume with VM object and cross-layer mount boundary,
not a claim that the buffer scan itself establishes exclusive ownership.
