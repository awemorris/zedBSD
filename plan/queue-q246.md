# Queue q246: xHCI IRQ measurement

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q245](queue-q245.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p030](ws025-io-memory-cache/phase030-imod-measurement/phase.md) | uncleared | Driver IRQ entry/owned/event counters; guest rate collection; native 4000 cell and supported builds |

HAL proposal remains untouched. No IMOD default change or physical latency claim.

Result: IRQ entry/owned/event counters and collector implemented. amd64 disk-image
and guest compilation pass; collector rate/boundary/invalid-input checks pass.
Native 4000 and PCAT/PC98 gates deferred when user approved the HAL redesign;
no measured IRQ rate claimed. Resume the native cells after HAL migration.
