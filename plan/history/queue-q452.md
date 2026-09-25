<!-- awesome-plan project=zedbsd record=queue-history q452 -->

# Queue q452: sh の展開の拡張（ws065-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q452
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「…もし/bin/shに実装しても逸脱にならないなら、この機会に実装してしまうのがいいと思います。」範囲は [ws065-p002](../ws065/phase002/phase.md)（判断は [WS065](../ws065/ws.md) の表）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q452-i01 | [ws065-p002](../ws065/phase002/phase.md) | cleared（`${v:o:l}`、`${v/p/r}`・`//`・`/#`・`/%`、`${v^}`・`^^`・`,`・`,,`、`${!v}`。bash の参照 21/21、dash との差は `${x//}` の 1 件だけ増） |

依存: ws065-p001（cleared）。

Upcoming Work Outlook: ws065-p003（builtin）、規約（ws065-p004・ws061-p011・ws064-p003・ws063-p002、最後）。
