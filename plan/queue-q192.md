# Queue q192: UAS synchronous endpoint transport

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q191](queue-q191.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Connect high-speed command state to four preallocated synchronous endpoint URBs, bounded shared deadline, terminal failure admission, and checked teardown; focused host ownership/sequence tests and supported builds |

Disk publication, task management recovery, SuperSpeed streams and native UAS
persistence/replug remain in the full phase. No driver match before those owner
contracts are connected. This queue implements the reusable transport, not a
replacement for native acceptance.

Result: synchronous endpoint transport implemented and focused production tests
pass ordinary/ASan/UBSan; all three supported image builds pass. The full phase
remains uncleared pending class/disk ownership, task recovery, streams and native
acceptance. See phase results for evidence and next implementation boundary.
