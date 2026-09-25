<!-- awesome-plan project=zedbsd record=queue-q366 -->

# Queue q366: rpi4 の build と QEMU での login（ws036-p012）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザー指示「rpi4 カーネルはビルドできるように修正して、qemu でログインプロンプトが表示されるところまでもっていきましょう」と「作業を継続してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q366-i01 | [ws036-p012](../ws036/phase012/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q366-i01 | ws036-p012 | **cleared**。kernel・userland を amd64 と同じ include の分離と kcrt へ、SD の lock、serial の console driver、FAT＋UFS の SD image。QEMU raspi4b で login prompt と login、`dyntest` PASS、8/8 起動。途中で見つけた sched の起動時の競合（全 platform）も直した。前提の p001・p025・p024・p010・p011 の rpi4 分も同じ作業で cleared。HAL は不変 |
