<!-- awesome-plan project=zedbsd record=queue-q360 -->

# Queue q360: root の UFS image を BUILD ごとに（ws034-p052）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 夜間・自走の指示（見つけた不具合は Phase にして直す）と 2026-09-24「作業を継続してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q360-i01 | [ws034-p052](../ws034/phase052/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q360-i01 | ws034-p052 | **cleared**。root の UFS image を共有の `build/arch-images` から `$(BUILD)/arch-images` へ。別の config の BUILD の root を取り込む不具合（sshd が消えた）が再現手順で起きないこと、pcat の起動を確認 |
