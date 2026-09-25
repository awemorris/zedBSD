<!-- awesome-plan project=zedbsd record=queue-q349 -->

# Queue q349: 開発用ファイルの option と base の既定（ws034-p039）

Status: finished（2026-09-24）
Executor: メインセッション（サブエージェントは使わない）
Approval: ユーザーの夜間の自律実行の指示（2026-09-23）と、2026-09-24 の「自走を再開してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q349-i01 | [ws034-p039](../ws034/phase039/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q349-i01 | ws034-p039 | **cleared**。menuconfig の「Install development files」（`n` で header・`.pc`・`.so` の link・`.a` を除く）。base の既定を全部 ON にし、amd64 でしか build できない 9 個の platform を直した（既定の i386 は以前から壊れていた）。既定の選択を platform で絞る |
