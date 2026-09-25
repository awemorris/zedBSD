<!-- awesome-plan project=zedbsd record=queue-history q444 -->

# Queue q444: make を host と同等に（ws061-p008）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q444
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「makeの時間はまだ遅いので、こちらも同等を目指しましょう。また、configureの方も、おそらくページキャッシュを最適化すれば、Linuxホスト並みにできると思いますので、目標を維持して取り組んでください。」範囲は [ws061-p008](../ws061/phase008/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q444-i01 | [ws061-p008](../ws061/phase008/phase.md) | cleared（make 12.8〜13.1 秒、host 15.2 秒） |

Upcoming Work Outlook: configure の page cache、system call の `syscall`/`sysret`、libc の lock の adaptive spin（2026-09-26 ユーザー提案）、ws063-p002（規約と回帰）、F-016。
