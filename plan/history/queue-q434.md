<!-- awesome-plan project=zedbsd record=queue-history q434 -->

# Queue q434: private の file の mapping で page cache の page を直接 map する（ws046-p014）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q434
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「expatのconfigureとコンパイルがLinuxと同等水準になることを直近の目標」「新規実装よりもバグ修正とパフォーマンス改善」「USB ECMよりもconfigureパフォーマンス改善を優先してください」。範囲は [ws046-p014](../ws046/phase014/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q434-i01 | [ws046-p014](../ws046/phase014/phase.md) | uncleared（kernel の変更は完了して適用: file fault 8.4 → 3.2 µs、`cc t.c -o t` 0.35 → 0.25 秒、configure（tmpfs）35 → 30〜33 秒、`mprotect` の commit の穴を直した。expat の make status 0、runtests 4932/4932。受け入れの「check が status 0」は base に bash が無く `tests/xmltest.sh` が動かないため未達。判断待ち: runtests の全 pass で満たしたとみなすか、bash を足すか） |

依存: ws046-p012（cleared）。

Upcoming Work Outlook: ws061-p003（fault-around）、ws061-p004（overlay の同期書き）、ws056-p001 の判断（BUG-046）、ws046-p014 の判断（check と bash）。
