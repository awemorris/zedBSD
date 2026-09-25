<!-- awesome-plan project=zedbsd record=queue-q358 -->

# Queue q358: 即時起床の設計（ws041-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「スレッドの即時wakeは実装しましょう」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q358-i01 | [ws041-p001](../ws041/phase001/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q358-i01 | ws041-p001 | **cleared**。[design.md](../ws041/design.md): 遅れは同じ CPU の起床だけ（i386 で最大 50 ms）。user へ戻る地点で、同じか高い優先度の起床が押しのける。押しのけられた thread は先頭へ。quantum は起床で満たさず 10 ms。HAL 変更なし |
