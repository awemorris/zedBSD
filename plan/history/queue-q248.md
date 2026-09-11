# Queue q248: shared system page operations

Date: 2026-09-10
Status: finished
Authorization: explicit approval of complete HAL proposal
Timebox: 90 active minutes
Previous: [q247](queue-q247.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](../ws025/phase037/phase.md) | uncleared | PA-capable query across HALs, kernel range API, amd64 SYS operations with shared table ownership; focused tests and builds |

Common VM lifetime/consumer migration follows. Unsupported dynamic system mapping
architectures report an empty allocation range rather than a false capability.

Result: PA query and kernel range APIs implemented, amd64 SYS operations share
existing page-table ownership. Focused actual operation/VM/scratch checks and
three supported builds pass. Common VM consumer migration and native acceptance
remain; hal_vmap_* has not yet been removed, so p037 stays uncleared.
