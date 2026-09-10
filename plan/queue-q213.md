# Queue q213: UAS removable-media control owner

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q212](queue-q212.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Readiness worker, old-medium retirement and reprobe, checked join before detach/quiesce; targeted host checks and build |

Native in-place exchange and partition discovery remain required follow-up acceptance.

Result: control worker and serialized medium replacement implemented. Actual-source
host tests pass normally and under ASan/UBSan. SS removable I/O, removal/replug and
checked four-CPU halt pass; amd64/PCAT/PC98 builds pass. Full phase remains uncleared
for native in-place/empty-medium acceptance, partition rediscovery and stopped
removable transport recovery. See phase results for evidence and resume conditions.
