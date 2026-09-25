<!-- awesome-plan project=zedbsd record=queue-q421 -->

# Queue q421: coreutils を zedBSD 向けに cross build（ws046-p008 の残り）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー判断「coreutilsはクロスビルドできればそれでいいです。」範囲は [ws046-p008](ws046/phase008/phase.md) の残り（host で zedBSD 向けに configure・build・install）。
Timebox: ユーザーの朝 9 時頃まで。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q421-i01 | [ws046-p008](ws046/phase008/phase.md) | uncleared（libc の header の誤り 2 つを直した。mount の一覧の API が無く止まった → p013） |

依存: なし。人間の判断は要らない。

Upcoming Work Outlook: ws046-p012（private の mapping の共有の設計）、HAL の rdmsr の差分（承認待ち）、WS055。
