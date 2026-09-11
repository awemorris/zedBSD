# Queue q252: common VM contiguous map runs

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution and approved HAL consolidation
Timebox: 90 active minutes
Previous: [q251](queue-q251.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](../ws025/phase037/phase.md) | uncleared | Populate frames before mapping; group adjacent PA runs; rollback tests and supported builds |

No HAL API additions. Native timing and DMA acceptance follow this bounded increment.

Result: physical-run grouping implemented, focused ordinary/sanitizer tests and
three final builds pass. First PCAT width warning corrected. Native timing and
DMA acceptance remain; full phase uncleared.
