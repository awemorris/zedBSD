<!-- awesome-plan project=zedbsd record=queue-q402 -->

# Queue q402: kernel の mkdir(2) の `.`（ws046-p006、BUG-032）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS046 の計画。範囲は [ws046-p006](../ws046/phase006/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q402-i01 | [ws046-p006](../ws046/phase006/phase.md) | cleared（BUG-032 resolved） |

依存: ws046-p002（cleared）。人間の判断は要らない（POSIX の errno、kernel で HAL ではない）。

Upcoming Work Outlook: ws046-p003（automake の idiom と GNU の機能）、p004（実 package）、WS047 p001、WS045。

結果: ws046-p006 cleared、BUG-032 resolved。kernel の mkdir(2) が最後の成分の `.`・`..` に EINVAL でなく EEXIST を返すようにした（path が引けるとき）。
guest で `mkdir -p ./a` などが通り、make の差分試験の automake の case が guest でも 15/15。boot test に BUG-031 が 2 回目に出た。
