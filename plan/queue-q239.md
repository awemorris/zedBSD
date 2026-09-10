# Queue q239: retain readable input aliases

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q238](queue-q238.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Input-specific write-protect lease, actual VM regression/refault checks, supported builds and repeated native comparison |

Retain full-unmap lease semantics for future output isolation; default remains
off until measured benefit and complete acceptance justify adoption.

Result: input-only protection and write-fault restoration pass focused/VM
fixtures, supported builds and native tests. CPU improves versus full-unmap
but is still slower than staging (3.87 s vs 3.08 s); default stays off.
