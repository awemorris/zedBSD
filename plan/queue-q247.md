# Queue q247: physical allocation argument expansion

Date: 2026-09-10
Status: finished
Authorization: explicit user request and full HAL proposal approval
Timebox: 90 active minutes
Previous: [q246](queue-q246.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](ws025-io-memory-cache/phase037-hal-interface-consolidation/phase.md) | uncleared | Remove request structure from physical allocation API, implementations and callers; focused allocation/VM checks and builds |

System-space and common VM migration follow this contract change.

Result: allocation request structure removed throughout active production/tests;
focused VM, mapping/scratch, allocator compatibility checks and all three builds
pass. p037 remains uncleared for SYS/query/common VM ownership and consumer migration.
