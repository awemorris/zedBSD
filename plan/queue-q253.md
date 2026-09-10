# Queue q253: native input after map coalescing

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution / approved common VM consolidation
Timebox: 90 active minutes
Previous: [q252](queue-q252.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](ws025-io-memory-cache/phase037-hal-interface-consolidation/phase.md) | uncleared | Current copy and coalesced input-view native cells, compare identical guest, restore ordinary build |

Native DMA remains separate. Pending trap decision is outside this queue.

Result: current pair passes, same guest. Copy CPU 2.87 s, view 4.68 s (previous
view 5.98 s). Default off retained; ordinary build restored. DMA/performance remain.
