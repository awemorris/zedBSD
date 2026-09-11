# Queue q108: owned requests and asynchronous BIO workers

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes, record findings and continue.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p019](../ws025/phase019/phase.md) | completed | Separate device wait from submission with bounded owned requests and independent workers. p009/p014/p015 and p017 complete. |

M/W/P, q107 results and current BIO, synchronous loop/BOT/NVMe callbacks,
thread creation and claim lifetime were inspected. p019 precedes p018 to provide
independent worker ownership before delayed writeback's device-isolation policy.
Selected finite implementation and acceptance are in the P book.
No commit, aggregate make check or .internal access. Serialize builds/tests;
make -j16 and disposable QEMU images. Physical gate remains user-accepted.

Previous: [q107](queue-q107.md), dirty/error/drain complete.
Next: p018 staged data writeback after the asynchronous ownership foundation.

Result: p019 complete; owner/cancel/loop/frontier tests, native scheduler/USB,
supported builds and FS50/Wi-Fi30/native acceptance pass. See P-book results.
