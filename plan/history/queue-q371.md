<!-- awesome-plan project=zedbsd record=queue-q371 -->

# Queue q371: sh の差分試験の土台と測定（ws042-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q371-i01 | [ws042-p001](../ws042/phase001/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q371-i01 | ws042-p001 | **cleared**。host build と dash との差分 runner（oils spec 1392 件）。我々の sh は 744/1392（53.4%）。失敗を展開・文法/実行・builtin に分類 |
