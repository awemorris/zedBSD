<!-- awesome-plan project=zedbsd record=queue-history q457 -->

# Queue q457: zdesktop の合成の核（ws035-p052）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q457
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「BUG-054の修正を最優先、ディスクイメージをnativeに変更したあと、ws035を次に優先。…これで進めてください。」と「そのあとで次の作業に進んで。」。p052 の範囲は 2026-09-25 のユーザーの承認（[compositing-design.md](../ws035/compositing-design.md)）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q457-i01 | [ws035-p052](../ws035/phase052/phase.md) | cleared（ウィンドウモードの Vulkan の合成と全画面モードの直接 scanout、切替。画面の読み取り 3 段が一致、import は buffer ごとに 1 回） |

依存: ws035-p051（cleared、設計は承認済み）。前の試行 sq001-i01 は uncleared（実装が残っていない）。

Upcoming Work Outlook: ws035-p053（`wl_shm` と cursor）、p054（acquire fence）… p058。
