# Queue q235: actual VM alias-lease acceptance

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q234](queue-q234.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Repair current-source VM/PTE fixture wiring; exercise real mutators with held lease, refault/COW and lifetime cleanup; fix discovered in-scope faults |

Result: actual VM mutator/lease host acceptance passes ordinary/sanitized
(11,073 checks each). No production change. p028 remains uncleared for
uaccess/syscall integration and native measurement. See phase results.
