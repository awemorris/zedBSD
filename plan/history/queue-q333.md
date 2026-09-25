<!-- awesome-plan project=zedbsd record=queue -->

# Queue q333: curses の termcap API（ws034-p041）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q332 finished（履歴 `plan/history/queue-q332.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。方針はユーザー決定（termcap は base の curses に）。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q333-i01 | [ws034-p041](../ws034/phase041/phase.md) | cleared | termcap API と `libcurses.a` の PIC 化 |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q333-i01 | ws034-p041 | **cleared**。ゲストで `TERMCAP PASS`。`libcurses.a` が PIE から link できなかった問題も直した |
