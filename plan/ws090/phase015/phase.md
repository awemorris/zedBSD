<!-- awesome-plan project=zedbsd record=ws090-p015 -->

# ws090-p015: Terminal・Notes の scroll を `kui_scroll` へ

Status: in-progress（q862-i01、P1、2026-10-08。下の「q862-i01」: libkeiland の kl_scroll の追加まで。Terminal・Notes の切り替えは WS113 の dock bar の後に再開）
Disposition: normal
Parent: [WS090](../ws.md)
Queue: q862 / q862-i01（P1）
依存: p011（cleared）

この file は 2026-10-01 に手引き（[../guide.md](../guide.md)）と一緒に作った。範囲は ws.md の表の行（Q1 の案）のとおりで、下の手順と完了の条件は
その行と p011 の「残り」から起こした案である。実行の前に main が範囲を確かめる。

## 範囲（ws.md の表、2026-09-30 Q1）

- Terminal（`userland/desktop/terminal/touch.c`）と Notes（`userland/desktop/notes/touch.c`）の指の scroll の model を、libkeiland の
  `keiland_scroller` の直の使用から libkeiui の `kui_scroll` に替える。
- `kui_scroll` に足す物: rubber band の境界（今の `kui_scroll_set_size` は 0〜（content − viewport）だけ。Terminal は y が負の向き（scrollback の
  上端 `top` < 0 〜 0）、Notes は x・y の 2 軸で rubber band の幅を viewport と別に渡す）と、位置の引き継ぎ（`keiland_scroller_set_position` の相当。
  app が他の手段（key・wheel・新しい出力）で view を動かした後、次の指が今の位置から drag を始める）。
- WS081 の host 試験 2 本（`plan/ws081/tests/run-termtouch.sh`・`run-notestouch.sh`）の compile の列の変更が要る（WS081 の file → **main に依頼**）。

## 手順（2026-10-01 追記）

`<W>` は `ws090-p015`。command は repo の root から。一般の build・boot test は [plan/ws104/commands.md](../../ws104/commands.md) の §1・§4。

1. 今の挙動の基準を取る（変更の前）:
   ```
   mkdir -p build/ws090-p015
   sh plan/ws081/tests/run-termtouch.sh build/ws090-p015/termtouch-before
   sh plan/ws081/tests/run-notestouch.sh build/ws090-p015/notestouch-before
   cp build/ws090-p015/termtouch-before/host-termtouch.log build/ws090-p015/termtouch-before.log
   cp build/ws090-p015/notestouch-before/host-notestouch.log build/ws090-p015/notestouch-before.log
   ```
   PASS: 最後の行 `host-termtouch: ...`・`host-notestouch: ...` に FAIL が無い（p011 の記録で 20 件・52 件）。
2. 今の使い方を読む（変える所の一覧）: `grep -n keiland_scroller userland/desktop/terminal/touch.c userland/desktop/notes/touch.c`。
   2026-10-01 の時点: Terminal は create・destroy・`set_position`（:132・:479）・`drag`（:271）・`step`（:275）・`set_bounds`（:455、
   `0.0, 0.0, top, 0.0, 1.0, height`: x は動かず、y は `top`（負）〜0、rubber band の幅 1・高さ `grid_height`）・`press`（:483）・`release`（:558・:582）・
   `cancel`（:570）。Notes は同じ組で、`set_bounds`（:975、`0, largest_x, 0, largest_y, width, height`）と `set_position`（:162・:490・:668・:1024・:1074）。
3. libkeiui に足す（`userland/desktop/libkeiui/scroll.c`・`include/libc/keiui.h`（ws104-p001 の後は `userland/desktop/include/keiui.h`）・`exports.map`）:
   - `void kui_scroll_set_bounds(struct kui_scroll *scroll, double minimum_x, double maximum_x, double minimum_y, double maximum_y, double band_width, double band_height);`
     （`kui_scroll_set_size` の一般の形。`scroll_bounds()`（scroll.c:550 付近）が今 `keiland_scroller_set_bounds(0, limit_x, 0, limit_y, width, height)` を呼ぶ所を
     この値で呼ぶ。`kui_scroll_limit_*`・`scroll_clamp` も最小の値を見る。`kui_scroll_set_size` は最小 0 のこの関数の呼び出しにする）。
   - `void kui_scroll_set_position(struct kui_scroll *scroll, double x, double y);`（glide を止め、scroller にも同じ位置を入れる。指が触れている間は何もしない、
     今の `kui_scroll_press`（scroll.c:302-318）の「触れていなければ位置を入れてから press」と同じ規則）。
   - `KUI_VERSION` を 1 つ上げる（2026-10-01 は 11 → 12。適用の直前に main の値を確かめ、main の最新の次にする）。header の版の列に 1 行。
4. libkeiui の host 試験に足す（`plan/ws090/tests/host-input.c`、WS090 の file）: 負の最小の境界での drag・overscroll・rubber band の戻り、`set_position` の後の
   drag の始まり、glide 中の `set_position`。`keiland_scroller` を直に使った場合と frame ごとに同じ位置になること（p003 の試験と同じ比べ方）。
5. Terminal・Notes の touch.c を `struct kui_scroll` に替える（gesture（`keiland_gesture`）と motion はそのまま。scroll だけ）。各 Makefile の依存と
   `platform/amd64/vmunix.mk` の link は p011 で `libkeiui` が入っている（変えない）。
6. WS081 の host 試験の compile の列に `userland/desktop/libkeiui/scroll.c` を足す変更を main に依頼する（`run-termtouch.sh`・`run-notestouch.sh` は
   `-Wconversion` で compile するので、scroll.c がそれで通るかを先に `cc -Wconversion -Werror -c` で確かめる。通らなければ libkeiui の側を直す）。
7. 確かめ:
   ```
   sh plan/ws090/tests/host-input.sh
   sh plan/ws090/tests/host-widgets.sh
   sh plan/tools/textedit/host-core.sh
   sh plan/tools/keiui/host-chooser.sh
   sh plan/ws081/tests/run-termtouch.sh build/ws090-p015/termtouch-after
   sh plan/ws081/tests/run-notestouch.sh build/ws090-p015/notestouch-after
   sh plan/ws079/tests/run-notes-host.sh
   ```
   PASS: `host-input: N/N passed`・`host-widgets: 94/94 passed`・`host-core: 34/34`・`host-chooser: 85/85 passed`、WS081 の 2 本が手順 1 と同じ件数で FAIL 0、
   `run-notes-host: ok`。
8. build（commands.md §1）warning 0。guest:
   - S8 の通し（WS079 の [guide.md](../../ws079/guide.md) §5.2 の 4 行、`demo-s8-s9: PASS`）。
   - WS081 の guest 試験の Terminal・Notes の touch（p011 が流した `p011`・`p013`・`p014`・`p015`。使い方は `plan/ws081/tests/` の各 script の先頭）。
   - Terminal の回帰 p079・p093・p100・p114・p086（と p016 で直した p088）を files の image で（[../guide.md](../guide.md) §5.4）。
9. 規約: `python3 plan/tools/style-check.py userland/desktop/libkeiui/scroll.c userland/desktop/terminal/touch.c userland/desktop/notes/touch.c` が 0、`git diff --check` 0。
10. boot test（commands.md §4、`OUTPUT=build/ws090-p015/boot`）。

## 完了の条件

1. Terminal・Notes の touch.c に `keiland_scroller_` の呼び出しが無い（`grep -c keiland_scroller userland/desktop/terminal/touch.c userland/desktop/notes/touch.c` が 0）。
2. 手順 7 の host 試験が全て PASS し、WS081 の 2 本の件数が前後で同じ。
3. 手順 8 の guest 試験が PASS（QEMU の Venus）。指の scroll・glide・rubber band の見た目が前と同じ（画面を並べる）。
4. build warning 0、style-check 0、boot test PASS。
5. 2026-10-10 までに終わらなければ merge しない（design.md J5。デモの Notes・Terminal を壊さない）。

## 未知

- `-Wconversion` の下で `scroll.c` が通るか（手順 6）: 2026-10-01 の scroll.c は通る（`clang -std=gnu11 -O2 -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion -Ibuild/ws090/inc -c userland/desktop/libkeiui/scroll.c -o /dev/null` が exit 0。`build/ws090/inc` は `sh plan/ws090/tests/host-input.sh` が作る）。足した関数の後にもう一度確かめる。
- Terminal の view は「行」の単位（`touch_position()` が行と pixel の offset から位置を作る）で、`kui_scroll` の bars（`kui_scroll_draw_bars`）は使わない。
  bars を使わないことで `kui_scroll` の `moved_us` の扱いに差が出ないか（見るのは `kui_scroll_step` の戻り値だけにする）。

## q862-i01（P1、2026-10-08、途中）

名前は今の tree に読み替える: `kui_scroll` → libkeiland の `kl_scroll`（`userland/desktop/libkeiland/ui/scroll.c`）、`keiland_scroller` → `kl_scroller`。
Q1 の承認（2026-10-08）: 範囲 1〜4、WS081 の `run-termtouch.sh`・`run-notestouch.sh` の compile の列に `ui/scroll.c` を足す 1 行ずつはこの Phase の所有に加える。

済み（commit は下の報告）:
- 変更前の基準: `run-termtouch.sh` 20 checks ok、`run-notestouch.sh` 52 checks ok（`build/p1-ws090-p015/*-before`）。
- `kl_scroll`（KL_VERSION 61）: `kl_scroll_set_bounds`（最小・最大と rubber band の大きさ、`kl_scroll_set_size` で元の端に戻る）、`kl_scroll_fling` が飛ぶかを返す、
  `kl_scroll_axis_at`・`kl_scroll_axis_stop_at`（event の時刻と速度、既存の `axis`・`axis_stop` はこれを now で呼ぶ）、`kl_scroll_axis_holding`、
  指が保持中に `kl_scroll_move_to` で位置が引き継がれた後の axis の取り直し、Home は最小の端へ。host（`-Wconversion`）と zedBSD の build で warning 0、
  style-check 0。位置の引き継ぎは `kl_scroll_move_to(glide=0)` で足りる（新しい関数は作らない）。
- host 試験の link: `ui/scroll.c` は `kl_canvas_round`・`kl_scroll_bar_*` を引くので、試験は `ui/scroll-bar.c`・`ui/canvas.c`（libc と libm だけ）も compile する（source の移動はしない）。

残り: Terminal・Notes の `touch.c`・`touch.h` を `struct kl_scroll` に、試験の compile の列（WS081 の 2 本、`plan/ws090/tests/host-pad.sh`）、`host-input.c` の case、Linux の build、T1 への依頼。
