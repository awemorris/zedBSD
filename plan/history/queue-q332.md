<!-- awesome-plan project=zedbsd record=queue -->

# Queue q332: 端末のモード切替で先行入力が失われる（ws035-p043）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q331 finished（履歴 `plan/history/queue-q331.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。試験の妨げになっていた不具合の修正。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q332-i01 | [ws035-p043](../ws035/phase043/phase.md) | cleared | `TCSETS` の ICANON 切替で溜まった入力を移す |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q332-i01 | ws035-p043 | **cleared**。host 試験（修正を外すと失敗）、ゲストの先打ち 5/5（修正前 0/5） |
