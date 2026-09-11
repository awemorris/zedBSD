# Queue q087: Request-sized regular-file syscall I/O

Date: 2026-09-06
Status: finished
Baseline: 8d9f418 (Improve FS)
Authorization: user's explicit request to fix syscall 4KiB backend splitting.
Timebox: progress review every 90 active minutes, inherited from q086.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws018-p020](../ws018/phase020/phase.md) | completed | Replace fixed 4KiB regular-file splitting with request-sized bounded allocation; q086 complete |

Prior results: [q086](queue-q086.md). No commit or aggregate make check.
Result: [q087 evidence](../ws018/phase020/results.md).
