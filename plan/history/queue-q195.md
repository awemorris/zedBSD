# Queue q195: UAS task abort and write uncertainty

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q194](queue-q194.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Checked host retirement followed by distinct-tag ABORT TASK, bounded response validation, next-request recovery without replay, sticky failed-write/flush status; focused fault tests and builds |

Task abort does not reset media, clear persistence uncertainty, or retry the
failed BIO. Stop never authorizes recovery. SuperSpeed and native injected faults
remain required; host fixtures are not substituted for their acceptance.

Result: bounded task abort and sticky write-uncertainty handling implemented;
focused production transport/class tests pass ordinary and ASan/UBSan, three
supported builds pass. Full phase remains uncleared pending native injected
recovery and the remaining stream/media/filesystem acceptance cells.
