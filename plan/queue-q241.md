# Queue q241: coherent read prefix contract

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q240](queue-q240.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Prefix-preserving coherent read API, forbid raw-buffer fallback, hostile backend/short/memory tests and supported builds |

Writable user views and read syscall integration follow this proven contract.

Result: prefix-safe coherent read primitive and hostile-backend controls pass
ordinary/sanitized tests (45,660 checks); three builds pass. File/uaccess/read
syscall output integration and native acceptance remain uncleared.
