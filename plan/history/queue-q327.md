<!-- awesome-plan project=zedbsd record=queue -->

# Queue q327: ps の CPU 時間の単位（WS040 の漏れ）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q325 finished（履歴 `plan/history/queue-q325.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23、`plan/master.md`「夜間の自律実行」）
Start UTC: 2026-09-23T12:30:00+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q327-i01 | [ws040-p006](../ws040/phase006/phase.md) | cleared | `/dev/system` の process 一覧が kernel の tick を運んでいた。`times()` と同じ固定単位にし、`ps` の TIME を直す |

## 範囲外

HALの変更。aggregate `make check`。commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q327-i01 | ws040-p006 | **cleared**。`ps` の TIME が 20 秒の CPU で `00:00:20`（amd64、1000 Hz） |
