<!-- awesome-plan project=zedbsd record=queue-history q426 -->

# Queue q426: 長さの 32 bit 切り捨て（BUG-048）の修正と `MAP_NORESERVE`（ws057-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q426
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…そうなっていなかったら…この機会に修正してほしいです。引き続きパフォーマンス問題の修正と、バグ修正と、上記観点での修正をお願いします。」（design policy 10）。範囲は [ws057-p002](ws057/phase002/phase.md)。HAL は変えない。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q426-i01 | [ws057-p002](ws057/phase002/phase.md) | cleared（`SYSCALL_PAGE_MASK` と vmspace の丸め 5 箇所を 64 bit に、`MAP_NORESERVE` を定義して受け付け。128 GiB の `PROT_NONE` の reserve が成功し commit は増えず、5 GiB の mapping の offset 4 GiB + 4 KiB を触れ、9 GiB は ENOMEM。回帰 boot PASS・sh 1388/1425（同じ集合）・make 91/91・SMP 0） |

依存: ws057-p001（cleared）。

Upcoming Work Outlook: ws057-p003（裏打ちの定義、ユーザーの判断待ち）、ws058-p001、ws056-p001 の判断（BUG-046）、ws046-p012、BUG-047、BUG-036・039・040・041、WS055。
