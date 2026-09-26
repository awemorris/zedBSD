<!-- awesome-plan project=zedbsd record=ws069p002 -->

# ws069-p002: Xzed の rootful の Wayland backend

Phase ID: `ws069-p002`
Parent: [WS069](../ws.md)
Status: cleared（q473-i01、2026-09-26）
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

## 結果（q473-i01、2026-09-26）

実装:

- `xzed/wayland.c`・`wayland.h`（新、style-check 0）: zwl への接続、xdg_toplevel（題名 X11、app_id Xzed）、wl_shm の XRGB8888 の
  buffer 2 枚（release で交互、全画面を写して変わった矩形を damage）、wl_seat の pointer（enter・motion・button → Xzed の pointer
  frame、button は X の 1・2・3）と keyboard（evdev → Xzed の keycode、Shift・Ctrl・Alt、caps lock）。pointer は zwl が描く。
- `xzed/glyphs.c`・`glyphs.h`（新、style-check 0）: libtruetype で等幅の TTF（`/usr/share/fonts/zdesktop-mono.ttf`）を 13 px で
  8x16 の cell の 1 bit に（baseline は cell の 12 行目、ASCII は cache）。`/dev/graphics` の glyph（レガシー）は使わない。
- `main.c`: `--wayland`、`initialize_graphics`（`/dev/graphics` の部分を切り出し）と `initialize_wayland`、poll に Wayland の fd、
  present の分岐（cursor を重ねない）、draw_text の glyph の分岐、cleanup。style-check の指摘は HEAD より 1 つ少ない。
- `input.c`: evdev → X の keycode を `xzed_input_keycode` として公開（`x_keycode` はそれを呼ぶ）。
- build: amd64 の Xzed は動的 link（libwayland-client・libtruetype）で `XZED_WAYLAND` を定義。他の platform（i386・PC-98 の
  静的な Xzed）は同じ file の stub（`--wayland` は ENOTSUP）で build できる（stub の compile を確認）。

確認（Venus、QEMU）: `plan/ws069/tests/x11-p002.sh`（`build/ws069-p002/run2/typed.png`）: zwl --glass の「X11」の窓に
`Xzed --wayland --size 800x500 -- /bin/zterm` の zterm（X の client）、click して打った `echo X11-OK; uname -a` の出力が
TrueType の文字で見える。build は warning 0。回帰 p052 PASS。

未実施・残り: pointer の click の X の client への到達は zterm では見えない（keyboard の focus は取れた）。X の cursor の形、
scroll、窓の大きさの変更（rootful は固定）、rootless（p003）、GLX（p004）。i915 実機は未実施。
