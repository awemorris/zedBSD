<!-- awesome-plan project=zedbsd record=queue-q388 -->

# Queue q388: guest の回帰（ws043-p011）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「続けてください。自走をお願いします。」と WS043 の計画。範囲は [ws043-p011](../ws043/phase011/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q388-i01 | [ws043-p011](../ws043/phase011/phase.md) | uncleared（guest 446/449。libc の `%.0f` の正確さと stdout の buffer の 2 つの欠陥。p012 の後に再開。BUG-029 を記録） |

依存: ws043-p008（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws043-p012（libc）、p011 の再開、p013（file の utility）で WS043 を終える。その後 WS042 p005・p006（実 script）、WS046（GNU make）、WS047 p001（build system の設計）。
