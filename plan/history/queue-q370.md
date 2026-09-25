<!-- awesome-plan project=zedbsd record=queue-q370 -->

# Queue q370: userland/base の動的リンク（ws034-p057）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザー指示「userland/base/ にスタティックリンクのバイナリが残っていたら、ダイナミックリンクに変更しておいてください。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q370-i01 | [ws034-p057](../ws034/phase057/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q370-i01 | ws034-p057 | **cleared**。amd64・pcat・pc98・rpi4 の base の program と Noct を PIE ＋ libc.so に。静的な実行 file 0。4 platform で login。起動の遅さは p058 |
