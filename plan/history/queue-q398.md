<!-- awesome-plan project=zedbsd record=queue-q398 -->

# Queue q398: 規約の全文照合 ls（ws042-p011）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS042 の計画。範囲は [ws042-p011](../ws042/phase011/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q398-i01 | [ws042-p011](../ws042/phase011/phase.md) | cleared（ls の違反 0。書き直す前と後で出力が同じ） |

依存: ws042-p005（cleared）。人間の判断は要らない（意味を変えない規約の書き直し）。

Upcoming Work Outlook: ws042-p012（最後の回帰と WS042 の完了）、WS046（GNU make）、WS047 p001、WS048（後回し）。

結果: ws042-p011 cleared。`userland/base/ls/main.c` を規約の全文に合わせて書き直した（禁止の comment 110 件と style-check.py の 96 件を 0 に、`goto` をなくし、長い関数を分けた）。
検証: 書き直す前と後の ls の出力と status が 255 件で同じ、host の utility の差分試験 492/492、amd64 の build（warning 0）と boot test。
BUG-030 の 3 回目の観測を ticket に足した。
