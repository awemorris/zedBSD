<!-- awesome-plan project=zedbsd record=queue-history q485 -->

# Queue q485: i915 実機での GLX の間欠の止まりの段（ws069-p007）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-27）
Active Queue: q485
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連、X11 と GLX）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q485-i01 | [ws069-p007](../ws069/phase007/phase.md) | uncleared（canceled: 2026-09-27 ユーザーの判断で X server を作り直す。止まった段は swap の中の XGetGeometry の返事待ち、libX11 の XPending を修正。続きは ws069-p010） |

依存: ws068-p006・ws069-p005（cleared）。
