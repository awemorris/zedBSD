<!-- awesome-plan project=zedbsd record=queue -->

# Queue q338: TCP の throughput（ws034-p044）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q338-i01 | [ws034-p044](../ws034/phase044/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q338-i01 | ws034-p044 | **cleared**（改善）。拒まれた segment を window update ですぐ送り直す。1 MiB が 0.7〜7.6 秒、4 MiB が 23 秒で完了。NewReno 風の回復は悪化したので戻した |
