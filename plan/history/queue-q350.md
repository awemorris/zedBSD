<!-- awesome-plan project=zedbsd record=queue-q350 -->

# Queue q350: HD Audio の既定を ON（ws035-p048）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「HDA はデフォルト ON で OK」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q350-i01 | [ws035-p048](../ws035/phase048/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q350-i01 | ws035-p048 | **cleared**。amd64 の既定を ON（menu と Makefile の両方）。既定の image で `/dev/dsp0`、HDA 無しでも起動 |
