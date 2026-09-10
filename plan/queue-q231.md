# Queue q231: direct-I/O vmap verification baseline

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 30 active minutes
Previous: [q230](queue-q230.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Reconnect actual vmap/scratch fixture to merged io.c; verify ownership/rollback baseline before borrowed-frame implementation |

No direct-user syscall enablement in this queue. Preserve production source;
run this focused fixture only, ordinary and ASan/UBSan. Record remaining lease,
borrowed mapping, output publication and measurement work as uncleared.

Result: focused ordinary and sanitized baseline passed (13,548 checks each).
Fixture repair complete; p028 remains uncleared for borrowed mapping, content
lease, syscall integration and native measurements. See phase results.
