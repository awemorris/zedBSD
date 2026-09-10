# Queue q215: initially empty UAS removable LUN

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q214](queue-q214.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Boot with an empty removable SCSI LUN, require class binding without disk publication, insert media and verify normal I/O at both speeds |

Result: initially empty HS/SS LUN boot, post-login insertion, single USB binding,
first disk publication and persisted I/O pass. No kernel changes. Full p029 remains
uncleared for partition rediscovery and stopped-removable-transport recovery;
resume conditions and evidence are in phase results.
