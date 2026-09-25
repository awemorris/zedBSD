<!-- awesome-plan project=zedbsd record=ws063 -->

# WS063: UFS の journal を既定にする（journal の無い image は mount の時に作る、`nojournal` の mount option）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG004
Related Milestones: MG002（fg011: configure の性能）
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q443（finished）
Resume point: p002（規約と回帰）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー指示: 「ジャーナルなしで作成したイメージも、マウント時にジャーナルを作り直すようにして、ジャーナルをデフォルトで有効にしてください。マウントオプションでジャーナルオフに対応しましょう。」

ws061-p006 で UFS を write cached にした結果、journal の無い volume は電源断の後に辿れない inode（と二重の割り当ての恐れ）が残る（ws061-p006 の強制終了の試験）。journal を既定にしてこれを無くす。今の journal は操作ごとに flush を待ち遅い（WS060、200 の作成 20.6 秒）ので、先に WS060 で速くする。

受け入れ: journal の無い image（今の native の root）を mount すると journal が作られ有効になる。`mount -o nojournal` で無効。強制終了の後に起動でき、volume の検査（`check-volume.py`）が UFS OK。configure（`/root`）が journal 無しの write cached（12.3 秒）と同程度。回帰（boot、make・sh の差分試験、SMP、COW、UFS の試験、512 MiB）。規約。

依存: WS060（p002 の設計、p003 の実装）。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws063-p001](phase001/phase.md) | journal の大きさを mkfs で記録（既定は ext4 の表で最大 128 MiB、指定で 1 GiB）、mount で `.ufs-journal` を再利用・確保し直し・作成、extent の一覧、名前の特別扱い（2026-09-26 ユーザー指示） | cleared（q443-i01。root 64 MiB・13 extent、1 GiB の作成 3.2 秒、確保し直し、名前の保護、crash の試験 UFS OK、configure 11.3 秒） | ws060-p003 |
| [ws063-p002](phase002/phase.md) | 強制終了の試験、計測、全体の回帰、規約の適合 | planned | p001 |
