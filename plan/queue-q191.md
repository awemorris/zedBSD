# Queue q191: UAS high-speed protocol engine

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q190](queue-q190.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Implement bounded command IU encoding and high-speed READY/data/status state transitions, validate actual QEMU wire layout with focused production-code tests |

This is the next transport component, not completion of p029. USB URB ownership,
disk publication, task management, SuperSpeed streams and native persistence /
replug acceptance remain required. No UAS class binding is published by this
queue. Failed protocol state cannot admit more data or accept later completion;
its eventual transport owner must quiesce host DMA and retire device tasks.

Result: command protocol component and focused tests implemented; three supported
image builds pass. Full p029 remains uncleared because USB/disk transport and
native acceptance remain. Continue from the implemented component; evidence and
next ownership steps are in the phase results, not a hardware blocker.
