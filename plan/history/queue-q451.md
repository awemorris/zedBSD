<!-- awesome-plan project=zedbsd record=queue-history q451 -->

# Queue q451: sh の構文の拡張（ws065-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q451
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「/bin/shの互換性をまず向上させてもらえますか？…もし/bin/shに実装しても逸脱にならないなら、この機会に実装してしまうのがいいと思います。」範囲は [ws065-p001](../ws065/phase001/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q451-i01 | [ws065-p001](../ws065/phase001/phase.md) | cleared（`$'...'`、`[[ ]]`、`function`、`(( ))`、`for (( ))`、`++`・`--`・`,`、`\|&`、`<<<`、`>& file`、`n>&m-`、`<( )`・`>( )`、UTF-8 の `${#s}`。BUG-054（`/dev/fd` と diff）を記録） |

Upcoming Work Outlook: ws065-p002（展開）、ws065-p003（builtin）、規約（最後）。
