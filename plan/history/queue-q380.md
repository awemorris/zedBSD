<!-- awesome-plan project=zedbsd record=queue-q380 -->

# Queue q380: grep を POSIX の全体で作り直す（ws043-p004）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」と「続けてください。自走をお願いします。」。fg010（WS035）の次の Phase は合成の設計（p051）の承認待ちなので、次の優先の WS043 を進める。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q380-i01 | [ws043-p004](../ws043/phase004/phase.md) | cleared（GNU grep と 52/52、guest 52/52、ASan 0） |
