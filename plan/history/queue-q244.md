# Queue q244: scalar output syscall integration

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q243](queue-q243.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](../ws025/phase028/phase.md) | uncleared | READ/PREAD output views behind default-off flag; prefix-safe fallback and counters; syscall stories, native output comparison and three builds |

Native comparison uses disposable images with the output flag off/on; input remains off.

Result: syscall stories and output-on native cell pass; experimental/default amd64
builds pass. Baseline comparison and PCAT/PC98 gates not run. User HAL_SPACE_SYS
review supersedes continuation: architecture duplication confirmed. See
[review](../ws025/phase028/hal-vmap-review.md).
Resume after planning shared HAL page operations and common kernel VA ownership.
