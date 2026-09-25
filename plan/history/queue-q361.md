<!-- awesome-plan project=zedbsd record=queue-q361 -->

# Queue q361: 起床の遅れの測定（ws041-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「スレッドの即時wakeは実装しましょう」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q361-i01 | [ws041-p003](../ws041/phase003/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q361-i01 | ws041-p003 | **cleared**。端末の echo の測定で p002 の起床の順の欠陥（起きた thread を先頭へ入れると LIFO になり、押しのけた thread が先に戻る）を見つけ、起床の FIFO の列で訂正。busy の横の echo 3.97 → 0.60 ms。WS041 completed |
