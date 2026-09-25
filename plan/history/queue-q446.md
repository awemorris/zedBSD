<!-- awesome-plan project=zedbsd record=queue-history q446 -->

# Queue q446: system call の入口を `syscall`/`sysret` に、libc の lock の adaptive spin（ws061-p010）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q446
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「system call の入口を int 0xc2 から syscall/sysret に替えるのは、優先でお願いします。libc の lock の adaptive spinも優先でお願いします。規約適合は最後でいいです。続けてください。」範囲は [ws061-p010](../ws061/phase010/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q446-i01 | [ws061-p010](../ws061/phase010/phase.md) | cleared（`syscall`/`sysret`、adaptive spin） |

Upcoming Work Outlook（ユーザーの優先順）: ws061-p009（`cc` を host 以上に。clang の静的 link）、WS064（make の `-j`・並列の性能）、規約の適合（最後）。
