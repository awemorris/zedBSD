# Queue q254: native fragmented DMA after HAL migration

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution / approved HAL consolidation
Timebox: 90 active minutes
Previous: [q253](queue-q253.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](ws025-io-memory-cache/phase037-hal-interface-consolidation/phase.md) | uncleared | Link-only SG probe, 8 GiB USB writeback/fsync/remount data check, ordinary restoration |

Use disposable images, retain high fragmented DMA evidence. Fix obsolete fixtures
only as needed. Trap proposal remains pending and outside this queue.

Result: native fragmented DMA and writeback/fsync/remount data checks pass. All
vectors are bits=32 due to PCAT root mask; requested high DMA unverified. Ordinary
build restored, no wrappers. Resume conditions in p037 results; no live QEMU.
