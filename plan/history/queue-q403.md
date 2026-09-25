<!-- awesome-plan project=zedbsd record=queue-q403 -->

# Queue q403: automake の idiom と GNU の機能（ws046-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS046 の計画。範囲は [ws046-p003](../ws046/phase003/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q403-i01 | [ws046-p003](../ws046/phase003/phase.md) | cleared（差分試験 91/91、host・guest） |

依存: ws046-p002（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws046-p004（実 package の build）、p005（規約と回帰）、WS047 p001、WS045、WS049〜WS052（優先度はユーザーの指示待ち）。

結果: ws046-p003 cleared。make に GNU の関数 35 個、target ごとの変数、VPATH・vpath、`$(eval)`、無い include の作成と makefile の作り直し（exec し直し）を足した。
差分試験 91/91（host・guest）。host で expat を我々の make で configure・build・check・install でき、install の file の一覧が GNU make と同じ。BUG-030 の 4 回目。
