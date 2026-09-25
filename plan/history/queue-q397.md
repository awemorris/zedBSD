<!-- awesome-plan project=zedbsd record=queue-q397 -->

# Queue q397: 規約の全文照合 sh（ws042-p010）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS042 の計画。範囲は [ws042-p010](../ws042/phase010/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q397-i01 | [ws042-p010](../ws042/phase010/phase.md) | cleared（sh の違反 340 → 0。差分試験 1417/1425 のまま） |

依存: ws042-p008（cleared）。人間の判断は要らない（意味を変えない規約の書き直し）。

Upcoming Work Outlook: ws042-p011（ls の規約）、p012（最後の回帰と WS042 の完了）、WS046（GNU make）、WS047 p001、WS048（後回し）。

結果: ws042-p010 cleared。`userland/base/sh` の全 file で style-check.py の違反 340 → 0（意味を変えない書き直し）。
style-check.py の誤検出（`local` builtin の関数名、struct の member）を直した。
検証: host の sh の差分試験 1417/1425、host の行編集の試験 33/33、amd64 の image の build（warning 0）と boot test。
