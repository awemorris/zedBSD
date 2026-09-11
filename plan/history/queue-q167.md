# Queue q167: portable FAT32 formatting

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q166](queue-q166.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p034](../ws019/phase034/phase.md) | completed | Four sector geometries, host faults/sanitizers, independent inspection/mtools and three target builds pass |

This prerequisite implements the ESP filesystem initializer.
p006 still owns FAT32/native UFS formatters and automatic installer layout;
p007 owns native installation.
BeUI remains mandatory p029 after text installation.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.

Previous results: [p033 acceptance](../ws019/phase033/results.md).
Block command admission and complete native installation follow this codec.

Results: [p034 acceptance](../ws019/phase034/results.md).
