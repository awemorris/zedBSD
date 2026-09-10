# Queue q238: native input-view comparison

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q237](queue-q237.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Disposable QEMU baseline/experimental input-view write/pwrite, append/limit/fsync/readback and counters/CPU comparison; fix demonstrated in-scope failures |

Result: both native variants pass functional acceptance and exact counters.
Expanded comparison shows slower view CPU (4.76 s vs 3.04 s); default stays
off. p028 remains uncleared for cost reduction/adoption and output publication.
