# Queue q207: UAS media-sense revocation

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q206](queue-q206.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Revoke published media on SCSI media/policy attention, refuse queued old BIOs; test actual class sense handling and builds |

Replacement-media publication follows existing old-reference retirement; this
queue closes stale admission but does not claim complete replacement support.

Result: SCSI attention revokes published media and stops queued old CDB admission;
actual-source host tests, native SS reset regression and all three builds pass.
Full phase uncleared: replacement-media publication/held-reference and remaining
uncertain-write acceptance still outstanding.
