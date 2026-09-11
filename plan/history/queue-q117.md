# Queue q117: exec snapshot input ownership

Date: 2026-09-07
Status: finished
Authorization: standing user approval to complete WS025 autonomously.
Timebox: review every 90 active minutes; start 2026-09-07 14:18 UTC.
Previous: [q116](queue-q116.md), finished with p021 completed.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p022](../ws025/phase022/phase.md) | uncleared | Implement and verify immutable-input content leases and canonical cache reads as the first owner for exec snapshots. Dependencies p015/p016 and existing p006 loader are complete. |

Follow snapshot-design.md. Mutable/stacked inputs retain existing snapshot behavior.
Verify cache ownership, overlapping read leases, refusal/EOF and all cleanup paths
before connecting shared VM pages. Technical questions about snapshot VM/COW and
retained backing are resolved in the phase before enabling that path; do not claim
text sharing from a successful cached copy. Run focused real file/VM tests, loader
regressions and supported builds as changes warrant. Full p022 remains required.
No commits, aggregate make check, .internal access or concurrent builds/runtimes;
use make -j16. Physical gate remains user-accepted, not agent-measured.

Outcome: immutable input/cache owner and ELF integration verified. Full p022 remains
uncleared until actual shared private pages/COW and full EXEC/CACHE acceptance.
