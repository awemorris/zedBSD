<!-- awesome-plan project=zedbsd record=ws069p008 -->

# ws069-p008: zdesktop-x11server（単体の rootless の X server、組み込める形）

Phase ID: `ws069-p008`
Parent: [WS069](../ws.md)
Status: in-progress（q488-i01）
Phase disposition: normal
Queue: q488-i01
承認: 2026-09-27 ユーザー「Wayland用のXサーバは、単体のプログラムとして実装しましょう。userland/base/zdesktop-x11serverとします。」
「rootlessのみでOKです。…再利用できるモジュラリティを保っておくと、あとで組み込みが楽です。」「続けてください。」
設計: [design.md](../design.md) §0

## 方式

`userland/base/zdesktop-x11server/`（`/bin/zdesktop-x11server`、amd64）。Xzed（`userland/X11/xzed`、`6873440a` の時点）の X の
protocol の核と ws069-p002〜p005 の Wayland・rootless・glyph・GLX を移し、組み込める形に分ける:

| file | 役割 |
| --- | --- |
| `x11server.h` | 外への interface: `x11server_create`（設定）・`x11server_pollfds`（待つ fd の列）・`x11server_dispatch`（readiness を渡す）・`x11server_stopped`・`x11server_destroy`。global な状態を持たない |
| `internal.h` | 内部の型（`struct server`・client・窓・pixmap・GC・font）と module の間の関数 |
| `main.c` | 引数、signal、`poll` の loop（上の 5 つの関数だけを使う） |
| `server.c` | server の作成・破棄、listen socket、client の接続と読み（要求の分け方）、fd の列と dispatch |
| `protocol.c` | 要求の処理（`request` の switch）、返事・event・error |
| `window.c` | 窓の木・pixmap・GC・描画（fill・text）・合成、find・hit |
| `rootless.c` | X の top-level と Wayland の窓の対応、present、focus・pointer・key の配送 |
| `wayland.c` | Wayland の接続、xdg_toplevel、wl_seat、wl_shm の buffer（rootful の部分は持たない） |
| `keymap.c` | evdev の key → X の keycode（Xzed の input.c の対応の部分だけ） |
| `glyphs.c`・`glx.c` | libtruetype の glyph、GLX 拡張（Xzed から） |

持たないもの: `/dev/graphics`・`/dev/input`・rootful（x11-p002 の試験も移さない）。表示は p008 では wl_shm（標準の Wayland）の
まま、Vulkan での表示は ws069-p011。

切り替え: `zdesktop-x11`（App Home）は `zdesktop-x11server` を起動、試験（x11-p003〜p005、zdesktop-p070・p071）、config、demo image、
実機の scenario。Xzed はこの Phase では触らない（p009 で戻す）。

## 受け入れ

1. build warning 0、新しい code は coding-style の全文（`plan/tools/style-check.py` の指摘 0）。global な可変の状態は main.c の
   signal の flag だけ。
2. Venus: x11-p003（rootless の zterm）・x11-p004（glxtest）・x11-p005（zgears、回る）・zdesktop-p070（App Home の X terminal と
   Gears）が新しい server で PASS。
3. i915 実機の `zdesktop-x11` の run で desktop・Gears・X terminal が出る（gears_turns は BUG-057、p010）。boot test。
