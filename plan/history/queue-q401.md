<!-- awesome-plan project=zedbsd record=queue-q401 -->

# Queue q401: POSIX make の核（ws046-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS046 の計画。範囲は [ws046-p002](../ws046/phase002/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q401-i01 | [ws046-p002](../ws046/phase002/phase.md) | cleared（POSIX の case が host・guest とも 45/45） |

依存: ws046-p001（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws046-p003（automake の idiom と GNU の機能）、p006（BUG-032、kernel の mkdir）、p004（実 package）、WS047 p001、WS045。

結果: ws046-p002 cleared。`userland/base/make` を作り（9 file、約 6000 行）、GNU make 4.4.1 との差分試験の POSIX の 45 件が host と guest の両方で同じ。
automake の idiom も host で 15/15。guest の automake の 1 件で kernel の不具合（mkdir(".") が EINVAL）を見つけ、BUG-032 と ws046-p006 にした。
