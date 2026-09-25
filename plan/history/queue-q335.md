<!-- awesome-plan project=zedbsd record=queue -->

# Queue q335: TCP で大きな write が届かない（ws034-p042）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q335-i01 | ws034-p042 | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q335-i01 | ws034-p042 | **cleared**。6つの原因を直した（ACK して捨てる、固定 window、window update で起きない、回復が遅い、close で控えが消える、blocking write が1秒で失敗）。100 byte〜256 KiB が届く。throughput は p044、VM の pin は p043 へ |
