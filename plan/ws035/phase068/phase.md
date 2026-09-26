<!-- awesome-plan project=zedbsd record=ws035p068 -->

# ws035-p068: zdesktop-terminal（zterm を Wayland と Vulkan へ）

Phase ID: `ws035-p068`
Parent: [WS035](../ws.md)
Status: planned
Phase disposition: normal
Queue: —
承認: 2026-09-26 ユーザー「userland/base/ztermに、X11のターミナルの実装があります。これをWayland+Vulkanに移植して、userland/base/zdesktop-terminalとして実装をお願いします。」
（zterm の実際の場所は `userland/X11/zterm`）

## 範囲

1. `userland/X11/zterm`（Unicode VT100、Xlib）の端末の部分（pty・VT100 の解釈・画面の格子）を保ち、表示を Wayland の窓
   （xdg-shell、Wiseman の浮いたタイトルバーの下）と Vulkan の描画（glyph の atlas を texture にして格子を描く）へ移す。
   文字は libtruetype で等幅の TTF（JetBrains Mono、OFL。Inter と同じく git に入れず、image へは試験の追加 file）を glyph の atlas に描く。
   `/dev/graphics` の font（zterm・Xzed が使うもの）はレガシー用なので使わない（2026-09-26 ユーザー「/dev/graphicsのフォントは使わないでください。それはレガシー用です。」
   「libtruetypeを使ってください。」）。
2. 入力は wl_keyboard（keymap は zwl が送るもの）と wl_pointer（範囲外なら最小限）。
3. `userland/base/zdesktop-terminal` として package にし、zdesktop の image に入れる。

## 受け入れ

1. Venus の guest の zdesktop で窓が開き、shell の prompt が出て、打った command（`ls` 等）の出力が見える（VNC の画面）。
2. i915 実機（capture）でも窓が描かれる（可能なら）。
3. build は warning 0、新しい C は coding-style の全文を適用し style-check 0。
