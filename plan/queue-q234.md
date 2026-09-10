# Queue q234: private user-page alias lease

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q233](queue-q233.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Bounded pin-vector deduplication, all-alias reservation, synchronous PTE revoke, rollback/release fixture and supported builds |

No syscall enablement. Full VM entry-point races and native measurements
remain acceptance requirements after the controlled ownership fixture.

Result: alias lease implementation and controlled ordinary/sanitized fixture
pass; all three disk-image builds pass. p028 remains uncleared for actual VM
mutator races, syscall integration and native measurements. See phase results.
