# Queue q249: common kernel mapping ownership

Date: 2026-09-10
Status: finished
Authorization: explicit approval of HAL consolidation
Timebox: 120 active minutes
Previous: [q248](queue-q248.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](../ws025/phase037/phase.md) | uncleared | Common VM VA/frame/lifetime owner; scratch/DMA/uaccess migration; remove HAL vmap and lookup; focused owner tests and builds |

Native DMA/output acceptance follows owner migration if not completed in this queue.

Result: common owner and all three consumers migrated; old HAL APIs/file removed.
Focused common-owner/DMA/uaccess/VM tests, three builds and native output-enabled
cell pass. Default restored. Native owned-frame/DMA/input acceptance and map-run
optimization remain; p037 uncleared. Results distinguish slower output timing.
