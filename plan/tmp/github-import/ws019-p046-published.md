<!-- awesome-plan project=zedbsd record=ws019-p046 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase046/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p046: shared native installation layout

Status: completed q179; see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase046-native-layout-plan/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20), prerequisites p032/p034/p037/p045
Timebox: 60 active minutes

Implement a pure Noct plan used by both future frontends. Accept the observed
whole-disk identity and measured loader/kernel bytes, conservative source-tree
bytes/object counts, swap bytes and three caller-supplied distinct GUIDs. No
device writes, UUID generation or frontend confirmation in this module.

Reject read-only, partition, file-backed and source-overlapping targets; check
signed 64-bit arithmetic before every multiplication/addition. Native UFS
currently requires 512-byte device sectors; reject other geometry explicitly.
Use 1-MiB aligned partition starts and root length. ESP is at least 64 MiB,
or loader+kernel plus 1/32 for FAT tables and 16 MiB headroom, rounded to MiB,
whichever is larger. Reject ESP sectors exceeding UINT32_MAX. This exceeds
the FAT32 minimum cluster requirement at 512-byte sectors. Root takes the rest
before the backup GPT array/header, rounded down. Reserve conservative root
space for measured bytes plus one 8-KiB allocation per object, configured swap
and 64 MiB filesystem/free-space headroom. Refuse undersized disks before writes.
The displayed layout must be exactly the command layout, with ESP and native
UFS only; swap is a root file. GUID creation and collision inventory remain
transaction integration responsibilities, not hard-coded constants.

Return typed partition extents and argv groups for existing diskpart init,
plus native boot configuration selected by root/ESP PARTUUID. Test minimums,
large disks beyond 2 GiB, exact fits, overflow, bad geometry, GUID aliasing and
source overlap in host Noct interpreter and JIT. No placeholder installed UI.
p006/p007 remain open until this planner is connected to actual confirmation,
provisioning/copy/publication and source-free native boot acceptance.

This arithmetic plan is not codec admission: final native mkfs geometry and
capacity must be checked before destructive provisioning. Expose that explicitly
as requiresFormatAdmission in the result. Current boot swap preparation only
supports FAT boot slots or raw partitions, before native root mounts; do not
emit an invalid swap0=/swapfile. Root-file activation after mounting root remains
integration work, and the plan returns its separate path and size.
