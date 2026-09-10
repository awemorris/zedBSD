# Queue q240: batched input alias protection

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q239](queue-q239.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Group contiguous compatible input aliases, preserve failure restoration, actual VM range tests, supported builds and same-workload native comparison |

After the build gates, compare q238/q239 workload on fresh default and
experimental copies. No default adoption or output publication claim from
this queue alone.

Result: range batching, VM tests, all builds and native semantics/counters
pass. CPU is still above staging (3.43 s vs 3.06 s), so default remains off.
p028 remains uncleared, including unimplemented output publication.
