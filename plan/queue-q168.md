# Queue q168: reserved FAT32 mkfs command

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q167](queue-q167.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p035](ws019-installation/phase035-fat32-command/phase.md) | completed | Reserved mkfs, native FAT mount/copy/remount, shell cp repair, host regressions and three builds pass |

This prerequisite implements the ESP filesystem initializer.
p006 still owns FAT32/native UFS formatters and automatic installer layout;
p007 owns native installation.
BeUI remains mandatory p029 after text installation.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.

Previous results: [p033 acceptance](ws019-installation/phase033-partition-administration/results.md).
Block command admission and complete native installation follow this codec.

Previous codec results: [p034 acceptance](ws019-installation/phase034-fat32-formatter/results.md).

Accepted: [p035 results](ws019-installation/phase035-fat32-command/results.md).
UFS native geometry and automatic installer integration remain open.
