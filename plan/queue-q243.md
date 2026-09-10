# Queue q243: writable uaccess view

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q242](queue-q242.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Writable pin view with full alias lease, dirty accounting, shared rollback; focused tests and supported builds |

Syscall connection and native acceptance remain subsequent work.

Result: writable view implemented; ordinary/sanitized/unsupported-HAL focused
checks and three supported builds pass. p028 still requires syscall/native
integration and measurement; it remains uncleared.
