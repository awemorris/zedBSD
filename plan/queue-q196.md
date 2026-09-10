# Queue q196: native UAS timeout recovery

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q195](queue-q195.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Throttle only disposable UAS backing, force a read timeout, remove delay and require a new read plus captured distinct-tag ABORT TASK/Response; fix demonstrated defects |

Native evidence supplements q195 host fixtures. SuperSpeed, media generation and
mounted-filesystem acceptance remain. Keep source images unchanged and capture
fault injection, guest error, USB protocol and post-recovery data.

Result: native read timeout, distinct-tag task abort/TMF COMPLETE, and new read
with matching data PASS. Source image unchanged. Full phase remains uncleared for
streams, media/live generation, uncertain-write and filesystem acceptance.
