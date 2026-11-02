<!-- awesome-plan project=zedbsd record=ws345-p013 -->

# ws345-p013 libkeiland の resize の口（描画の thread のため）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §6.3。
- 状態: planned

## 目的

shell の描画の thread が窓の thread と別に present しても、resize が正しく見えるようにする libkeiland の口（design.md の敵対的レビューの指摘 4）。

## file

- `userland/desktop/include/keiland/keiland_window.h`・`libkeiland/ui/window.c`（Wayland）: configure の ack を listener の中でなく、program が新しい大きさの最初の present の直前に頼める口（`kl_window_configure_take`・`_ack`。ack は窓の thread の post で行い、描画の thread はその完了を待ってから present する）。口を使わない program は今のまま listener で ack する。
- `libkeiland/win32/window.c`: system の modal loop の間の大きさ（`WM_SIZE`・`WM_SIZING`）を `KL_WINDOW_RESIZE` で知らせる口（選ぶ program だけ）。
- `KL_VERSION` を上げ、既定は今の動き。libkeiland は他の WS の物でもあるので、Q1 が所有を調整する。

## 試験

- libkeiland の host の試験（Wayland の configure の順: ack の前に新しい大きさの commit が無い）、Win32 の Wine で modal loop の間の resize の通知。
- `sh plan/ws334/tests/cross-build.sh`、Keiland の app の回帰（Q1 が選ぶ既存の試験）。

## 受け入れ

- 口を使わない program の動きが前と同じ。使う program で、Wayland の ack と present の順が守られ、Win32 で modal loop の間に大きさが届く。
