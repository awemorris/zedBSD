<!-- awesome-plan project=zedbsd record=ws060 -->

# WS060: UFS の journal の commit を batch にして名前の操作を速くする（BUG-040）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG004
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q441（finished）
Resume point: 目標（BUG-040）は達成。規約の適合は WS063-p002 で WS060 の変更も含めて行う。2026-09-26 ユーザー指示（journal を既定に、WS063）で再開
<!-- awesome-plan-current:end -->

## 目標

[BUG-040](../bugs/BUG-040.md): journal の volume で名前の操作が 1 回約 100 ms（`journal_flush()` の `disk_sync()` = device 全体の flush が操作ごと）。
group commit（複数の操作を 1 回の flush にまとめ、短い timer か閾値で commit、`fsync`/`sync` は即時）にして、journal の volume の 200 の作成を journal 無しの数倍以内にし、続けた操作の間も guest が応答する。

受け入れ: BUG-040 の受け入れ案。crash の試験（`plan/tools/ufs/crash-test.sh`、途中で切っても volume が UFS OK で prefix が保たれる）、`check-volume.py`、sh・make の差分試験、boot。

きっかけ: ws054-p003（q415）で発見。ユーザーの指示（パフォーマンス問題の修正）。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws060-p001](phase001/phase.md) | 操作ごとの flush の数と費用の実測、group commit の設計 | uncleared（q431-i01、撤回。fg011 の後に再開） | — |
| [ws060-p002](phase002/phase.md) | batch の redo journal（v3）の設計。write cached と両立、journal を既定にする前提（WS063） | cleared（q440-i01。pin した metadata、1 秒の commit、2 slot の交互、最新の 1 つの replay、`/.zedjournal`） | p001 の実測 |
| [ws060-p003](phase003/phase.md) | v3 の実装、BUG-040 の受け入れ、crash の試験 | cleared（q441-i01。200 の作成 20.6 → 0.36 秒、crash の試験 4 時点と root で UFS OK。既定の有効化・mount の時の作成・`nojournal` も入れた。規約の指摘は ws063-p002） | p002 |
