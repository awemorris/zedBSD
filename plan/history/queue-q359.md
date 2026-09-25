<!-- awesome-plan project=zedbsd record=queue-q359 -->

# Queue q359: 即時起床の実装（ws041-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「スレッドの即時wakeは実装しましょう」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q359-i01 | [ws041-p002](../ws041/phase002/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q359-i01 | ws041-p002 | **cleared**。起床の preempt（同じか高い優先度、user へ戻る地点・tick・通知）、押しのけた thread は先頭へ、quantum 10 ms。busy process の横の pipe の往復が amd64 10 ms → 0.14 ms、pc98 100 ms → 2 ms、公平さ維持。HAL 変更なし。root の UFS image が BUILD 間で共有される build の不具合を発見（新 ws034-p052） |
