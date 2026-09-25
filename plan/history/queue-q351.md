<!-- awesome-plan project=zedbsd record=queue-q351 -->

# Queue q351: `FD_SETSIZE` を 1024 へ（ws034-p048）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「FD_SETSIZE はほかの POSIX と同様の数値に」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q351-i01 | [ws034-p048](../ws034/phase048/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q351-i01 | ws034-p048 | **cleared**。`fd_set` 1024 bit、kernel は nfds までの語だけを扱う（旧 4 byte の set も壊さない）。ゲストで 10/10。descriptor の上限 32 は新 ws034-p051 |
