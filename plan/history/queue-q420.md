<!-- awesome-plan project=zedbsd record=queue-q420 -->

# Queue q420: private の file の mapping で page cache の page を map する（ws046-p011）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー指示「ゲストでconfigureすると遅すぎるようです。その原因を探って修正する必要があります。」の続き（ws046-p010 の設計の実装）。範囲は [ws046-p011](ws046/phase011/phase.md)。
Timebox: ユーザーの朝 9 時頃まで。届かなければ uncleared で記録する。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q420-i01 | [ws046-p011](ws046/phase011/phase.md) | uncleared（compile と link は速くなったが configure 128 → 270 秒・make の失敗で戻した。p012 で設計を直す） |

依存: ws046-p010（cleared）。HAL の変更は要らない。

Upcoming Work Outlook: HAL の rdmsr の差分（承認待ち）、ws046-p008 の残り（coreutils の cross build）、WS055。
