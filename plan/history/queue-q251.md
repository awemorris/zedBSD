# Queue q251: migrated input-view native acceptance

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution and approved HAL consolidation
Timebox: 90 active minutes
Previous: [q250](queue-q250.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](../ws025/phase037/phase.md) | uncleared | Current default input-copy baseline, experimental input view, native data/accounting acceptance, restore ordinary build |

One disposable QEMU cell per variant using identical guest source. Record hashes,
CPU/wall/copy/view metrics and scope limits. No trap/syscall API changes.

Result: both native input cells pass with identical guest; copy/view attribution
correct. On CPU 5.98 s versus off 2.89 s, so default remains off. Ordinary build
restored. p037 retains native DMA acceptance; p028 retains performance work.
