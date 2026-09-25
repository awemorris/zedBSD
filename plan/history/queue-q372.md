<!-- awesome-plan project=zedbsd record=queue-q372 -->

# Queue q372: sh の展開（ws042-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q372-i01 | [ws042-p002](../ws042/phase002/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q372-i01 | ws042-p002 | **cleared**。lexer の語の終わり、展開・算術・括弧式の書き直し、echo。dash との差分 744 → 897/1392 |
