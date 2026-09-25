<!-- awesome-plan project=zedbsd record=queue-q393 -->

# Queue q393: guest での sh の差分試験（ws042-p006）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS042 の計画。範囲は [ws042-p006](../ws042/phase006/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q393-i01 | [ws042-p006](../ws042/phase006/phase.md) | cleared（guest 1390/1425、guest だけの失敗 84 → 25 を全て分類。seq・tac・whoami・egrep・fgrep・env を足し、sleep・head・ls・sh を直した） |

依存: ws042-p005（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws042-p007（対話の回帰）、WS046（GNU make）、WS047 p001、WS048（RPi4 の USB、後回し）。
