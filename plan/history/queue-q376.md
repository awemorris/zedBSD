<!-- awesome-plan project=zedbsd record=queue-q376 -->

# Queue q376: sed を POSIX の全体で作り直す（ws043-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: q377
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」
WS043 は WS042 の実 script（p005）の前提（configure が guest の sed・grep・awk を使う）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q376-i01 | [ws043-p002](../ws043/phase002/phase.md) | uncleared（2026-09-24 のユーザーの新しい依頼（rpi4 の console の font）を先にするため中断。`sed.h`・`main.c`・`Makefile` の下書きは `plan/ws043/phase002/draft/`、`compile.c`・`execute.c` は未着手。再開: q377 の後に新しい attempt で下書きから続ける） |
