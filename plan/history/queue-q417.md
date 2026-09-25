<!-- awesome-plan project=zedbsd record=queue-q417 -->

# Queue q417: guest で coreutils を最後まで（ws046-p008 の再開）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー「すべて承認します。」「それまで可能な限り自走してください。」範囲は [ws046-p008](ws046/phase008/phase.md) の残り（coreutils の configure・build・install、check の分類）。expat・zlib は q412 で済み。
Timebox: ユーザーの朝 9 時頃まで。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q417-i01 | [ws046-p008](ws046/phase008/phase.md) | uncleared（coreutils の configure が 20 分で 951 check。ユーザーの指示で止め、coreutils は cross build に、configure の遅さは p009 に） |

依存: WS054（completed）。人間の判断は要らない。

Upcoming Work Outlook: WS055（clang の既定に `--undefined-version`）、ws046-p009・p005。
