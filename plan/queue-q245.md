# Queue q245: approved HAL API naming correction

Date: 2026-09-10
Status: finished
Authorization: explicit user rename request and standing autonomous execution
Timebox: 60 active minutes
Previous: [q244](queue-q244.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p036](ws025-io-memory-cache/phase036-hal-space-api/phase.md) | completed | All 18 approved API/type/flag renames, active test migration and focused build gates |

Other HAL API additions and vmap architectural replacement are outside this queue.

Result: 18 names migrated across 44 files; active old-identifier scan clear;
actual VM and syscall focused checks pass; amd64/PCAT/PC98 builds exit 0.
Subsequent pmem signature and system-space work remains in the separately
[reviewed proposal](ws025-io-memory-cache/hal-interface-proposal.md).
