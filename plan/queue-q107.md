# Queue q107: dirty ownership, error observers and explicit drain

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes, record findings and continue.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p017](ws025-io-memory-cache/phase017-dirty-error-drain/phase.md) | completed | Preserve dirty ownership and independently observable errors; carry explicit through/drain context. p011/p013/p014/p016 complete. |

M/W/P, q106 evidence and actual file/VM/inode/mount/buffer/disk/loop interfaces were
inspected. Execute the selected staged integration and focused verification in the
P book. Maintain write-through until p018 explicitly enables eligible delayed data.
No commit, aggregate make check or .internal access. Serialize builds/tests;
make -j16 and disposable QEMU images. Physical gate remains user-accepted.

Previous: [q106](queue-q106.md), shared budgets and clean reclaim complete.
Next dependency-ready work after p017: p018 staged data writeback.

Result: p017 complete; focused normal/sanitizer, supported builds, FS50/Wi-Fi30
and native cache/USB acceptance pass. See the P-book results for exact evidence.
