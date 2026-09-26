<!-- awesome-plan project=zedbsd record=ws069p002 -->

# ws069-p002: Xzed の rootful の Wayland backend

Phase ID: `ws069-p002`
Parent: [WS069](../ws.md)
Status: in-progress（q473-i01）
Phase disposition: normal
Queue: q473-i01
承認: 2026-09-26 ユーザーの自律実行の指示
設計: [design.md](../design.md) §1・§2

## 範囲

1. `xzed/wayland.c`: zwl への接続、xdg_toplevel（題名 X11）、wl_shm の buffer 2 枚、wl_seat の pointer・keyboard。
2. `xzed/glyphs.c`: libtruetype の等幅 font（`/usr/share/fonts/zdesktop-mono.ttf`）の glyph を 8x16 の cell の 1 bit に。
3. `main.c`: `--wayland`（`/dev/graphics`・`/dev/input` を開かない）、poll に Wayland の fd、present と draw_text の分岐。
   `input.c` の evdev → X の keycode を関数として公開。
4. package: Xzed を zdesktop の image に（Wayland の backend のときは libwayland-client と libtruetype を link）。

## 受け入れ

1. Venus の zdesktop で `Xzed --wayland -- /bin/zterm`（または zshell）の窓が出て、X の app の text（zterm の shell の prompt）が見える。
2. keyboard で打った command の出力が見え、pointer で click が届く。
3. build は warning 0、新しい C の style-check 0（既存の main.c・input.c の変えた部分は HEAD より増やさない）。
