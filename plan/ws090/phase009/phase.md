<!-- awesome-plan project=zedbsd record=ws090-p009 -->

# ws090-p009: Files と Settings の描画の層を libkeiland へ（canvas・text・icons）

Status: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-261 で files-regress・files-p018・desktop-guest・settings-regress PASS、T1-272 で files-desktop-guest PASS。ユーザーが見る撮影は T1-373 (4)（t1 の worktree build/t1-373/shots/）、前の T1-266 との並べての比較は未）（旧: planned（2026-10-06 P1 が作成、q817））
Disposition: normal
Parent: [WS090](../ws.md)、設計 [design.md](../design.md) §10
Queue: q817（2026-10-06 ユーザーの決定「今 libkeiland の canvas へ移す」）
依存: WS131 p020（Files の窓が `kl_app`）・p019（Settings の窓が `kl_app`）、main に統合済み
所有 path: `userland/desktop/files/`、`userland/desktop/settings/`、`userland/desktop/libkeiland/ui/icons*.c`・`keiland-ui.h`（icon を 1 つ足す）、`plan/ws090/`、
Files・Settings の host 試験の compile の列（`plan/tools/files/`・`plan/ws089/tests/`・`plan/tools/settings/`）

## 出典

2026-10-06 ユーザー:「テキスト入力のあるすべてのアプリで、IMEを受け付けることをチェックしてください。…特に理由がなければlibkeilandのUIパーツを使ってほしいです。」
と、Files・Settings の欄の扱いの判断（クリック）「今 libkeiland の canvas へ移す」。q816 の (a) の暫定の IME は p010・p007 で kl_field に置き換える。

## 今の形（2026-10-06）

- Files は自前の描画の層（`files/canvas.c` 1414 行・`text.c` 739 行・`icons.c` 351 行、`canvas.h`）を持ち、Settings はその 3 file と `artwork/mark.c` を source のまま
  compile している（F-038）。
- libkeiland の `kl_canvas`・`kl_text`・`kl_icon`・`kl_image` は ws090-p002 でこれを写した物で、関数の形と struct の並びは同じ（2026-10-06 に比べた。違いは
  libkeiland の text の emoji と glyph の pixels の追加、icon の並びに Files の `FM_ICON_TODAY` が無いこと）。

## 範囲

1. libkeiland に `KL_ICON_TODAY`（Files の sidebar の Today）を足す（enum の最後、Files の線の絵を写す）。
2. Files と Settings の source の描画の層の名前を libkeiland の物に（`fm_canvas*`・`fm_text_*`（描画の関数）・`fm_icon*`・`fm_image*`・`fm_rect`・`fm_color`・
   `fm_glyph`・`FM_RGB(A)`・`FM_ICON_*` → `kl_*`・`KL_*`）、`files/canvas.c`・`text.c`・`icons.c`・`canvas.h` を消し（Q1 の rm の pipeline）、Makefile
   （zedBSD・Linux・FreeBSD）から除いて libkeiland を link する。Settings の Makefile から files の 3 file を除く。
3. host の試験の compile の列を libkeiland の `ui/canvas.c`・`text.c`・`icons.c`・`icons-line.c` に替える。
- scroll（Files の自前の scroller）の `kl_scroll` への置き換えは範囲の外（後の Phase、慣性は既に libkeiland の scroller）。

## 受け入れ

- Files・Settings に描画の層の `fm_`・`FM_` の名前が無く、`files/canvas.c`・`text.c`・`icons.c` が無い（F-038 の解消）。
- host の絵が前と同じ（files-render・settings-render の前後の比較。libkeiland の text の差（emoji・Mahora の companion）で違う所は理由を記録）。
- build（zedBSD warning 0、keiland-linux の gcc と clang）、`keiland-os-boundary/check.sh` PASS、Files・Settings の host 試験。
- QEMU（T1）: Files と Settings の画面、files-regress・settings-regress。
