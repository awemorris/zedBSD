# Queue q189: IMOD storage measurement

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q188](queue-q188.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](ws025-io-memory-cache/phase030-imod-measurement/phase.md) | uncleared | Add a bounded native write/fsync/readback workload and guest latency/CPU accounting to the existing isolated IMOD comparison; run QEMU cells and preserve the production default |

p031 is cleared. p028's VM alias/content ownership implementation remains
substantial; p029 still requires UAS transport. Advance the already implemented
p030 comparator with its defined next software step before returning to these
implementation phases. No physical tuning claim or expanded test cleanup.

Result: QEMU 0/160/4000 write/fsync/readback measurements and HID overlap pass.
p030 remains uncleared for the explicitly listed wider measurement cells;
production default remains 4000. See p030/results.md. Next select ready WS009
installation documentation while retaining WS025 implementation work.
