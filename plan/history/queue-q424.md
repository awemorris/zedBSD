<!-- awesome-plan project=zedbsd record=queue-history q424 -->

# Queue q424: libc に mount の一覧の API（ws046-p013）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q424
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「報告してくれた未修正の問題と、新しいバグについて、解決に取り組んでください。」（q421 で報告した coreutils の cross build が mount の一覧の API で止まる問題）と「しばらく自走してください」。範囲は [ws046-p013](ws046/phase013/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q424-i01 | [ws046-p013](ws046/phase013/phase.md) | cleared（libc に `<mntent.h>` の getmntent 系、加えて coreutils の cross build に要った `<elf.h>`・`<stdio_ext.h>`・`<utime.h>`・`fseeko`・spawn の `_np`・errno 14 個・`statvfs.f_basetype`。coreutils は configure・make・install が通り 102 program、guest で `df`・`stat`・`ls` などが動く。回帰 boot PASS・make 91/91・sh 1388/1425（同じ集合）。`df` の既定の出力の欠けは BUG-047） |

依存: なし（kernel の `KERN_SYSTEM_GET_MOUNTS` はある）。

Upcoming Work Outlook: ws056-p001 の判断（BUG-046）、ws046-p012、BUG-036・039・040・041、WS055。
