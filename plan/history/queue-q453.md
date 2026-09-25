<!-- awesome-plan project=zedbsd record=queue-history q453 -->

# Queue q453: sh の builtin の拡張（ws065-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q453
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「…もし/bin/shに実装しても逸脱にならないなら、この機会に実装してしまうのがいいと思います。」範囲は [ws065-p003](../ws065/phase003/phase.md)（判断は [WS065](../ws065/ws.md) の表）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q453-i01 | [ws065-p003](../ws065/phase003/phase.md) | cleared（`source`、`let`、`test ==`、`declare`・`typeset`、`printf -v`・`%q`、`builtin`、`pushd`・`popd`・`dirs`。bash の参照 33/33、dash との新たな差 6 件は F-019） |

依存: ws065-p001（cleared）。

Upcoming Work Outlook: 規約（ws065-p004・ws061-p011・ws064-p003・ws063-p002、最後）。
