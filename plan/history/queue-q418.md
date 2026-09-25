<!-- awesome-plan project=zedbsd record=queue-q418 -->

# Queue q418: guest の configure が遅い原因を調べて直す（ws046-p009）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー指示「ゲストでconfigureすると遅すぎるようです。その原因を探って修正する必要があります。漫然と長時間configureやビルドを実行しないでください。」範囲は [ws046-p009](ws046/phase009/phase.md)。
Timebox: ユーザーの朝 9 時頃まで。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q418-i01 | [ws046-p009](ws046/phase009/phase.md) | uncleared（原因 2 つを直した: file の fault 145 → 25 µs、configure 129 秒。残りは HAL の rdmsr（承認待ち）と p010） |

依存: ws046-p007（修正 1・2）。HAL の変更が要れば差分ごとに承認を求める。

Upcoming Work Outlook: ws046-p008 の残り（coreutils の cross build）、WS055。
