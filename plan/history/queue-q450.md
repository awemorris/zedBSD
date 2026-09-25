<!-- awesome-plan project=zedbsd record=queue-history q450 -->

# Queue q450: `/bin/sh` の process の生成（ws064-p004）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q450
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「/bin/shのプロセス生成も、vforkやposix_spawnになっているか確認して、最適化をお願いします。」範囲は [ws064-p004](../ws064/phase004/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q450-i01 | [ws064-p004](../ws064/phase004/phase.md) | cleared（pipeline の外部 command を posix_spawn、`echo`・`printf` を shell で pipe へ、command substitution を shell から、`unset` だけの subshell を shell で試して戻す。fork: make 1417 → 214、configure 966 → 423。configure 7.1〜7.25 秒、`make -j4` 4.18〜4.38 秒） |

Upcoming Work Outlook: 規約の適合（ws061-p011・ws064-p003・ws063-p002、ユーザーの指示で最後）。
