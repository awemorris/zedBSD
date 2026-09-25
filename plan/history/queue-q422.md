<!-- awesome-plan project=zedbsd record=queue-history q422 -->

# Queue q422: HAL の `amd64_percpu_current()` の差分を適用（ws046-p009 の再開）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q422
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「まず、承認待ちのHALの変更を許可します。次に、報告してくれた未修正の問題と、新しいバグについて、解決に取り組んでください。」範囲は [ws046-p009](ws046/phase009/phase.md) の残り（HAL の差分の適用と回帰）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q422-i01 | [ws046-p009](ws046/phase009/phase.md) | cleared（HAL の差分を適用、syscall 1340 → 450 ns、configure 129 → 96 秒。回帰は boot・sh 1388/1425・make 91/91・対話 41/41・SMP 0） |

依存: HAL の差分の承認（済み）。

Upcoming Work Outlook: ws046-p012（private の mapping の共有の設計を直す）、ws046-p013（libc の mount の一覧）、BUG-034〜041。
