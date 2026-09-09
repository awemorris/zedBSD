# Queue q169: native UFS initialization

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q168](queue-q168.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p036](ws019-installation/phase036-native-ufs-codec/phase.md) | completed | Empty native namespace, wide geometry, sparse/legacy/fault checks and three builds pass |

This prerequisite implements native UFS initialization and wide geometry.
p006 owns automatic disk provisioning; p007 owns native installation.
BeUI remains mandatory p029 after text installation.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.

q169 accepted [p036 native codec](ws019-installation/phase036-native-ufs-codec/results.md).
Public native command admission and QEMU mount/copy remain next; complete
installer and graphical frontend are not yet accepted.
