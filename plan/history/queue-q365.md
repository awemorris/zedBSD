<!-- awesome-plan project=zedbsd record=queue-q365 -->

# Queue q365: USB hub driver（ws035-p046）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザー指示「USB ハブドライバは作成したいので、phase を作ってください」と「作業を継続してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q365-i01 | [ws035-p046](../ws035/phase046/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q365-i01 | ws035-p046 | **cleared**。core の hub の port の一般化、xHCI の route string・TT・Hub、`usb-hub` driver。QEMU の xHCI・UHCI で hub の下の列挙・storage の読み・hot-plug・2 段・子から先の切り離し |
