<!-- awesome-plan project=zedbsd record=queue-history q435 -->

# Queue q435: fault-around（ws061-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q435
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「expatのconfigureとコンパイルがLinuxと同等水準になることを直近の目標」ほか。範囲は [ws061-p003](../ws061/phase003/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q435-i01 | [ws061-p003](../ws061/phase003/phase.md) | uncleared（fault-around と TLB の invalidation の縮小: file fault 1.4 µs、`clang --version` の fault 1/3、`cc t.c -o t` 0.21〜0.24 秒、configure 31 秒（tmpfs）・55 秒（overlay）。目標の 1/8 は未達。BUG-051（sshd の SIGSEGV 1 回）未解決。回帰 make 91/91・sh 1388/1425・COW・SMP） |

途中のユーザー指示: 「オーバレイファイルシステムでこれ以上の改善が見られない場合、ディスクイメージを変更して、vmunixをEFIシステムパーティションに置いて、rootfsはUFSのパーティションにしましょう。スワップは単独パーティションにしましょう。」→ WS062 を立てた。BUG-052（tmpfs 32 MiB）を記録。

Upcoming Work Outlook: ws062-p001（native の layout の image）、BUG-052、BUG-051、ws056-p001・ws046-p014 の判断。
