<!-- awesome-plan project=zedbsd record=queue-history q445 -->

# Queue q445: `cc t.c -o t` を host と同等以上に（ws061-p009）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q445
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「ccの実行時間もホストと同等以上に速くなることを目標にして、改修を進めてください。また、makeは-jに対応させて、並列makeの実行時間もホストと同等以上にしてください。」範囲は [ws061-p009](../ws061/phase009/phase.md)。この後に WS064（make の `-j`）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q445-i01 | [ws061-p009](../ws061/phase009/phase.md) | uncleared（着手の前に中断: 2026-09-26 ユーザーの優先の変更。p010 の後に再開） |

Upcoming Work Outlook: WS064 p001（make の `-j`）・p002（並列の性能）、規約の適合（WS060・WS061・WS063）、`syscall`/`sysret`、adaptive spin。
