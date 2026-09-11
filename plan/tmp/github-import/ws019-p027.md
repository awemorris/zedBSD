<!-- awesome-plan project=zedbsd record=ws019-p027 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase027/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p027: installation source selection and mounted-image admission

Status: completed q162; [acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase027-installation-source-selection/results.md), 2026-09-09
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

Add an early source-selection screen before install-mode/destination selection.
Only installation disk is selectable now. Keep HTTP as a future source provider,
unavailable until installer network configuration and HTTP acquisition are
implemented; do not offer an operational HTTP route or silently use a network.

Selecting installation disk requires proof that rootfs.img is actually mounted
from the installer disk. If absent, zedinst must report that the system was not
booted from the installer disk and stop before any destination mutation. A file
named rootfs.img merely existing on disk, or a configuration string naming it,
does not satisfy this requirement.

Inspect the live root/overlay lower mount and loop backing identity together
with retained physical boot provenance. The current root-image mount can be
private and absent from ordinary mount output; expose the required read-only
facts through an existing general-purpose command (mount/sysctl) if necessary.
Do not mistake a missing public mount-point pathname for an absent private
rootfs mount. Match the backing file/partition to the actual installer source;
reject unrelated lookalikes, replaced source, native-root boot and stale records.

After admission, p007 can expose the immutable image through an additional
owned read-only mount point for attribute-preserving tree copying. Returning
from the installer shell rechecks source and destination observations.

Acceptance: ordinary installer USB with private mounted rootfs succeeds;
native-root boot or absent rootfs mount fails with the requested diagnostic;
mere/unrelated rootfs.img, changed source and query failures are refused without
writes. Disk is the only selectable source; HTTP cannot proceed. Add to a
finite queue after the current p026 copy acceptance, then integrate the source
screen with p006's coexistence/dedicated mode selection and update WS009.

Visual regression found in q161's actual installer screenshot: Term.open leaves
a literal `4;2m` prefix above the title. Inspect console CSI handling against the
exact Noct terminal initialization sequence when integrating the source screen;
consume unsupported controls correctly rather than printing their parameter
tail. Capture the corrected actual screen and verify input/close restoration.
Evidence: ../temp/q161-find1/installer-confirmation.png (current coexistence UI).
