<!-- awesome-plan project=zedbsd record=ws019-p034 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase034/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p034: portable FAT32 initializer

Status: completed q167; timebox 90 active minutes
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20), prerequisite of p006 ESP formatting

Implement userland-only FAT32 geometry, bounded metadata generation and exact
readback on an already admitted descriptor. Accept 512/1024/2048/4096-byte
sectors, checked 32-bit sector totals and FAT32 cluster limits, cluster sizes
up to 32 KiB. Choose a sufficient FAT length without unbounded iteration.
Create two FATs, reserved clusters, root cluster 2, FSInfo and backup boot
records. Zero all metadata and root entries; preserve unallocated data bytes.
Do not publish a successful boot record before FAT/root writes have flushed.
Report short/failed I/O and flush failures without claiming rollback.

No kernel private sources, external formatter execution or product-only test
switches. The caller owns device admission, expected identity and final close.
The later mkfs frontend must acquire BLKRESERVE before invoking this codec.
This phase does not expose an unprotected block formatter command.

Acceptance: boundary geometry and overflow, dirty preimages, complete metadata
readback and corruption rejection, write/read/flush faults, preserved unused
data, independent Python parsing and mtools list/copy/read interoperability.
Host ordinary and sanitizer builds; compile codec for amd64/pcat/pc98 using
their user ABI. Native mkfs command and mounted ESP acceptance belong to the
following admission/integration phase. Kernel FAT currently only accepts
512-byte sectors; other codec geometries are not claimed mountable in zedBSD.

Reference: [Microsoft FAT32 specification 1.03](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf).

Accepted 2026-09-09: [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase034-fat32-formatter/results.md). Command admission and native mount
testing remain the next prerequisite; this completes the portable codec only.
