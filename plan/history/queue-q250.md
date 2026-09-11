# Queue q250: common VM native owned-frame acceptance

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution and explicit HAL consolidation approval
Timebox: 60 active minutes
Previous: [q249](queue-q249.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](../ws025/phase037/phase.md) | uncleared | Link-only migrated owner probe, disposable 8 GiB / 4 CPU QEMU, frame rollback/sharing/scratch; restore ordinary image |

Trap/syscall interface consolidation awaits the user's stack-contract decision and
is outside this queue. No new HAL interfaces. Broader DMA/input acceptance remains.

Result: native owned-frame rollback, high fragmented mappings, user-space/AP
visibility, forced scratch fallback and root login pass. Ordinary build restored
and no wrapper symbols remain. Full p037 awaits native DMA/input acceptance.
