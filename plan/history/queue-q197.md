# Queue q197: USB stream identity contract

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q196](queue-q196.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Add explicitly gated URB stream identity and default-stream reset semantics, test actual USB core, map xHCI stream-ring ownership changes before enabling capability |

No HCD advertises stream support until ring allocation, completion provenance,
cancel/dequeue and teardown support are implemented. This API is the first piece
of that integration, not SuperSpeed acceptance.

Result: USB stream identity API and actual-core focused tests implemented; three
supported builds pass. xHCI integration sites and ownership contracts detailed.
Full phase stays uncleared; no HCD stream capability is advertised yet.
