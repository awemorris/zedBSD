<!-- awesome-plan project=zedbsd record=ws018-p018 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws018/phase018/phase.md`

親: [ws018](https://github.com/awemorris/zedBSD/issues/19)

# ws018-p018: Coherent filesystem I/O path

Date: 2026-09-06
Status: completed
Queue: q086
Authorization: user's explicit plan/50 scenarios/queue/execute request.
Scope: Bounded heap syscall batching preserving pipe/vector/VM contracts; validated loop logical extents through parent cache with FAT slot coherence; full-block UFS overwrite read avoidance.

Design and exclusions: [shared implementation plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/fs-report-implementation-1.md).
Acceptance: [50 stories](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/fs-acceptance-50.md).
Dependencies follow queue order. Results must distinguish host doubles, actual
production code, native QEMU and physical hardware. Save command/log evidence and
remaining work in results.md. No commit, no make check, make -j16 serialized.
Completion requires implemented scope and its applicable acceptance gates;
unrun native checks are not passes.


Evidence: [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws018-kernel-architecture/phase018-storage-io-path/results.md).
