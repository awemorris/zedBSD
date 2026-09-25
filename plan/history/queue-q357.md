<!-- awesome-plan project=zedbsd record=queue-q357 -->

# Queue q357: networkd の二重 DHCP（ws035-p047）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「networkd が DHCP のあとにすぐ DHCP を行う問題は、phase を作って修正してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q357-i01 | [ws035-p047](../ws035/phase047/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q357-i01 | ws035-p047 | **cleared**。QEMU の差し直しで再現（手の DHCP の直後に 2 回、何もしないと 0 回）。kernel に `RTM_IFINFO_ARRIVAL`、手の DHCP・static の成功を方針に記録、名前を継いだ adapter の raised を戻す。修正後は 6 回とも 1 回 |
