<!-- awesome-plan project=zedbsd record=queue-q352 -->

# Queue q352: shell の job 表と `kill %N`（ws034-p047）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「シェルの kill %N は修正してください」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q352-i01 | [ws034-p047](../ws034/phase047/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q352-i01 | ws034-p047 | **cleared**。job 表（`jobs.c`）、`kill` builtin、`%N`・`%%`・`%+`・`%-`、prompt での Done 報告。非対話・対話（11/11）とも確認。旧出力に依存した ws001 の試験を削除 |
