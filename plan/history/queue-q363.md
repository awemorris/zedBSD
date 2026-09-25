<!-- awesome-plan project=zedbsd record=queue-q363 -->

# Queue q363: libc の mkostemp・posix_fallocate（ws034-p053）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断に続く自走（p050 で見つけた不足）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q363-i01 | [ws034-p053](../ws034/phase053/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q363-i01 | ws034-p053 | **cleared**。`mkostemp`・`mkostemps`・`posix_fallocate`（ftruncate＋block ごとの 1 byte）。pipe・socket の `fstat` が EINVAL だった kernel の不足を直した。ゲストで 16/16 |
