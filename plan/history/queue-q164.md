# Queue q164: exclusive whole-disk administration

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q163](queue-q163.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p031](../ws019/phase031/phase.md) | completed | FD-owned whole-disk exclusion, claimed writes and owned reload, close cleanup; host/native/build acceptance |

This prerequisite implements the kernel boundary needed by destructive CLI
initialization. p006 owns command integration, formatters and automatic layout;
p007 owns native installation.
BeUI remains mandatory p029 after text installation.
[q164 results](../ws019/phase031/results.md):
host/sanitizer, real claim registry, disposable QEMU lifecycle and all three
builds pass. Next: connect GPT initialization to diskpart, then partition
formatters and dedicated installer integration.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.
