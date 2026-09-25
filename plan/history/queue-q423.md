<!-- awesome-plan project=zedbsd record=queue-history q423 -->

# Queue q423: BUG-034・035・037 の修正（ws056-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q423
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「次に、報告してくれた未修正の問題と、新しいバグについて、解決に取り組んでください。」範囲は [ws056-p001](ws056/phase001/phase.md)（BUG-034 の `RTSIG_MAX`、BUG-035 の `struct atomic_record`、BUG-037 の pax の `x`・`g`・`L`・`K`）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q423-i01 | [ws056-p001](ws056/phase001/phase.md) | uncleared（BUG-034・035・037・043・044 resolved、BUG-042 の真因（thread の signal mask の継承）を kernel と libc で resolved。pax は pax・gnu の archive を guest で展開して host と一致、`POSIX-R2-REMAINING.ELF` 01-12 PASS。残りは `POSIX-R2.ELF` が console で timer の試験の EINTR（BUG-046）で status 0 にならないことだけ。ユーザーの判断待ち） |

依存: なし。

Upcoming Work Outlook: ws046-p012（private の mapping の共有の設計を直す）、ws046-p013（libc の mount の一覧）、BUG-036・039・040・041、WS055。
