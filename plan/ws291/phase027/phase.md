<!-- awesome-plan project=zedbsd record=ws291-p027 -->

# ws291-p027 NVIDIA: libvulkan の submit の hand-off（B-U、design の P-A）

Status: in-progress・ラップアップ（2026-11-01 22:10 Q1。実装と host 試験 H3a・H3b、実機の段 A は済み: subs-1000 55.5/59.0 → 86.4/82.9 fps、subs-100 +32 %、画の比較 21 か所一致。段 B〜D と Venus の回帰、arm64・riscv64 の __thread の libvulkan の実行は未実施。branch agent/p1-ws291p026 の b675639e9 は main に merge しない（回帰の後）。結果は agent の branch の phase.md）

- WS: [WS291](../ws.md)。設計: [phase026/design.md](../phase026/design.md) の P-A（B-U）と [phase026/phase.md](../phase026/phase.md) の Phase の表・受け入れ。
- 範囲: design.md の P-A のとおり（0. GPU の 1 run を timing 無しで測り直す、1. B-U: hand-off・入口の drain・back-pressure・失敗の latch・fork・環境変数の切り、2. host 試験、3. 10.0.10.3 の RTX 2070 で subs-100・subs-1000 ほかの計測と回帰、Venus の回帰）。
- Q1 の判断（p026 の問い、2026-11-01 21:07）: fence の無い vkQueueSubmit は返事を待たずに返す（A。nvrtx だけ、環境変数で切れる。その submit の memory の不足は後で DEVICE_LOST になる）。mview の bench の mode（`--record=once`・`--submit-all`）は足す。
- 受け入れ: design.md の P-A の受け入れ（subs-1000 の fps が NVK の 75.4 に近づく見込みを実測で確かめる、後退なし）。
