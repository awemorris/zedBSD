# Queue q170: shared native UFS mkfs command

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q169](queue-q169.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p037](../ws019/phase037/phase.md) | completed | Shared device frontend, 4-GiB native format/mount/attribute copy, FAT regression and three builds pass |

This prerequisite connects native UFS to the shared protected mkfs command.
p006 owns automatic disk provisioning; p007 owns native installation.
BeUI remains mandatory p029 after text installation.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.

Accepted: [p037 results](../ws019/phase037/results.md).
Private source-image mount, native swap file and complete installer integration
remain next. The graphical frontend is not yet implemented.
