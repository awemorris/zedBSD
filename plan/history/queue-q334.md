<!-- awesome-plan project=zedbsd record=queue -->

# Queue q334: curl（ws034-p017）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q333 finished（履歴 `plan/history/queue-q333.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q334-i01 | [ws034-p017](../ws034/phase017/phase.md) | uncleared | curl と libcurl |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q334-i01 | ws034-p017 | **uncleared**。package・TLS・CA 検証は動く。libc（`fd_set`、`IN6_IS_ADDR_*`）と kernel（`FIONBIO`）の不足を直した。kernel の TCP が MSS を超える write を届けないため HTTPS の転送が終わらない。新しい Phase ws034-p042 を作った |
