<!-- awesome-plan project=zedbsd record=queue -->

# Queue q331: libc の iconv（ws034-p040）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q330 finished（履歴 `plan/history/queue-q330.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。方針はユーザー決定（iconv は libc に、ASCII と UTF-8 だけ）。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q331-i01 | [ws034-p040](../ws034/phase040/phase.md) | cleared | p005 から切り出した iconv |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q331-i01 | ws034-p040 | **cleared**。host 試験と glibc との突き合わせ（20万件、食い違いは glibc が緩いものだけ）、ゲストの smoke |
