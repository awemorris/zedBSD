<!-- awesome-plan project=zedbsd record=queue-q367 -->

# Queue q367: descriptor を 1024 まで（ws034-p051）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（`FD_SETSIZE` はほかの POSIX と同じ値に）と「作業を継続してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q367-i01 | [ws034-p051](../ws034/phase051/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q367-i01 | ws034-p051 | **cleared**。`KERN_OPEN_MAX` 1024、伸びる表、reservation の上限、batch の close-on-exec、poll・select の heap、`/dev/fd/NNNN`、`getdtablesize()`。guest 20/20、handle/fd の host 試験 PASS、4 platform の kernel warning 0 |
