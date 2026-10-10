<!-- awesome-plan project=zedbsd record=ws048-p010 -->
# WS048 p010: VL805起動時のDMA size不整合を修正

Status: cleared
Disposition: normal
Parent: [WS048](../ws.md)
Queue: [限定repair Queue](../../ws203/codex-repair-20261011.md)
Authority: current user「USBもなんかエラーが」「attach failed at controller start (3)」、既存USB修正指示の継続。
Scope: 非coherent DMA backingをpage単位で確保し、payload sizeはcaller要求を維持。現kern_pmemは要求sizeを保持し、uncached HALはpage-aligned rangeを要求する。xHCI scratchpad arrayは8*count bytesなのでEINVALになる。従来host allocatorはsizeを勝手にroundしてこの不整合を隠していた。
Criteria: 本番kern_pmemと同じsize契約の既存限定DMA host checkで旧source失敗/新source成功、coherent/uncached/欠落APIの挙動、size/boundary/overflowレビュー、現在config warning0 kernel build、最終C全文review。実機USB入力はp009/p006の受入に残す。
Boundaries: drv_dma/HAL public API変更なし。既存呼出のpage backingだけ修正、USB hardware rewriteなし。QEMU/toolchain/pushなし。新phaseを限定追加、旧p004/p009履歴は保持。
Standards: AGENTS.md、Guardrail、C全文/.clang-format、automation。WS203 p004と共通build。共有計画とremote publicationはQ1 pending。

## Terminal scoped result / 2026-10-11

限定source/build criteria cleared。[最終差分・commands・versions・旧/新対照・全文規約・limitations](../../ws203/tests/initialization-repair-20261011.md)。現在config warning0 arm64 kernel build、DMA/GENET既存host checksとGIC/MMIO限定check PASS。実機は未実施、親WSの受入未達を保持。先行Phaseの過去outcomeは書き換えない。main統合承認待ち、共有投影/GitHubはQ1 pending。

## Main integration / 2026-10-11

Current user「main仁藤剛してください。」を直前の7b16e364c統合承認への回答（mainに統合してください）として受領。clean main6b722f47eから修正7b16e364c53761f96a982797f88793f7f520dbb6へfast-forward統合、競合なし。先行read-only調査記録08e919a08も含む。main上で全8 source/test SHA256一致、source diffなし、現在config.mkが検証済みworktreeとbyte-identicalであることをread-back確認。前turnのwarning0 kernel buildと限定host checksが統合sourceに適用されるため追加のbuild/試験は行わない。sourceのmain統合は完了、先行の承認待ち表記は当時の履歴。新imageでの実機DHCP/SSH・USB入力受入は未達のまま、WS203/WS048はincomplete。pushなし。共有Master/Queue/history/FutureWork/GitHubはQ1投影保留。
