<!-- awesome-plan project=zedbsd record=queue-history q484 -->

# Queue q484: i915 実機での GL と X11（ws068-p006）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-27）
Active Queue: q484
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q484-i01 | [ws068-p006](../ws068/phase006/phase.md) | cleared（i915 実機で GLX の zgears・X terminal・仮想デスクトップ。実行器に vkResetDescriptorPool と動的状態の命令、libEGL の pbuffer の clear を pass で。回転の間欠の止まりは BUG-057） |

依存: ws068-p008・p010、ws069-p005、ws035-p070・p065（cleared）。
