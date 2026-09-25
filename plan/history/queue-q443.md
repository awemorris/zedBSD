<!-- awesome-plan project=zedbsd record=queue-history q443 -->

# Queue q443: journal の大きさの記録と mount での journal の file の管理（ws063-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q443
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「mkfsではジャーナルサイズだけ記録して、ジャーナルファイルはマウント時に再利用・再確保・作成などしましょう。ファイル名は.ufs-journalがいいです。…続けてください。」範囲は [ws063-p001](../ws063/phase001/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q443-i01 | [ws063-p001](../ws063/phase001/phase.md) | cleared（大きさの記録、mount での管理、`.ufs-journal`、extent） |

Upcoming Work Outlook: make を host と同等に（2026-09-26 ユーザー指示）、configure の page cache の最適化（目標は host 並み）、ws063-p002（規約と回帰）、system call の `syscall`/`sysret`、F-016。
