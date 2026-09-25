<!-- awesome-plan project=zedbsd record=queue-q379 -->

# Queue q379: sed を POSIX の全体で作り直す（ws043-p002、再開）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」と、同日の「続けてください。自走をお願いします。」
q376-i01 で中断した ws043-p002 を、`plan/ws043/phase002/draft/` の下書きから再開する。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q379-i01 | [ws043-p002](../ws043/phase002/phase.md) | cleared（GNU sed と 79/79、guest 79/79、ASan 0、expat・coreutils の configure が同一） |
