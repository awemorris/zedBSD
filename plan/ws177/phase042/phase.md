<!-- awesome-plan project=zedbsd record=ws177-p042 -->

# ws177-p042: PDF Viewer の選択（頁をまたぐ・語と行・全て・指・回転した字の塗り）（案 L の 3）

Parent: [WS177](../ws.md)
Status: test-wait（T1-474、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q907-i01（P1、承認は p040 と同じ）
Origin: [backlog-p2](../backlog-p2.md) 20・21・24 行（WS128 ws128-p004）、[案](../phasing-20261008.md) の L。

## 範囲と設計

- 20: 選択の両端を（頁、文字）の組にする（`select_page`・`select_anchor` が始まり、新しい `select_caret_page`・`select_caret` が終わり）。
  drag は pointer の下の頁（間の隙間なら近い頁）の近い文字まで。塗りは間の頁の全部と両端の頁の部分。copy は頁の順に並べ、頁の境は
  改行。ダブルクリック（400 ms・4 px の中の 2 回目の押し）は語（英数字の続き、漢字・ひらがな・カタカナはそれぞれ同じ種類の続き）、
  トリプルクリックは行（`LINE_END` の間）。Ctrl+A と Edit > Select All は文書の全て（copy の時に頁を読む）。copy は 4 MiB まで
  （越えたら越える前の頁まで、知らせ「Copied the first N pages」）。
- 24: 選択と一致の塗りを文字の四隅の多角形で（`pv_canvas_blend_quad`、画素の中心で内外を決める凸の四角形の塗り）。回転した頁・縦書き
  の文字も字の向きのまま塗る。軸に沿った字は今と同じ箱。
- 21: 指の選択（touch.c と find.c）: 文字の上の長押しで語を選び、両端に handle（丸）を出す。handle の上に指を置くと、その端を指の
  近い文字へ動かす（頁をまたいでよい）。選択の外を tap すると選択を外す。指の scroll・zoom は handle の外では今のまま。核の
  関数（`pv_select_word_at`・`pv_select_handle_at`・`pv_select_handle_move`）は find.c に置き、host で試す。

## 確認

- host: `plan/ws177/tests/host-pdf-find-l.sh` の select の群（ws128 の edit-basic.pdf と p041 の PDF）: 2 頁にまたがる drag と copy、
  ダブル・トリプルクリック、Ctrl+A の copy、長押しの語と handle の移動（頁をまたぐ）、tap で外す、回転した頁（edit-basic の 3 頁）の
  塗りの画素（多角形の外が塗られない）。frame を PNG に。
- build warning 0、style-check。指の実機は UAT。

## 実装と確認（2026-10-08 夜、P1 q907）

- `find.c`: 選択の終わりの頁 `select_caret_page`、`find_bounds`・`find_page_range`・`find_point`、ダブル・トリプルクリック（`click_*`、`find_word`
  の文字の種類・`find_line`）、`pv_select_all`（Ctrl+A、Edit > Select All、`(size_t)-1` は頁の最後）、copy は頁ごと（`find_copy_page`、頁の境は改行、
  4 MiB まで）、指の `pv_select_word_at`・`pv_select_handle_at`・`pv_select_handle_move`・`pv_select_clear`、handle の描画、塗りは
  `pv_canvas_blend_quad`（`canvas.c`、画素の中心で決める凸の四角形、1 画素より細ければ箱）。
- `touch.c`・`touch.h`: 長押し（`KL_GESTURE_LONG_PRESS`）で語、handle の上の指は gesture に渡さず handle を動かす（`touch_handle`）、tap で外す。
- `view.c`・`menu.c`・`viewer.h`: Ctrl+A（`PV_KEY_A`）、`PV_ACTION_SELECT_ALL`、Edit > Select All。
- log: `SELECT page= from= to= to-page=`（前の形の後ろに足した）、`SELECT word|line page= from= to=`、`SELECT all pages=`、`SELECT handle=`、`TOUCH long-press … selected=`、`TOUCH handle=`。
- 試験: host-pdf-find-l の select（2 頁から 3 頁への drag と copy「page two has a lazy dog\nlazy」、2 回で「quick」、3 回で行、長押しで「brown」と
  handle、終わりの handle で「brown fox」、始まりの handle を越えて入れ替わる、tap で外す）と turned（30 度の form の T の真ん中は塗られ、
  箱の角は塗られない）。frame は `build/p1-l/run/frames-plain/*.png`（select-02-handles・turned-01-all を見た）。

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-pdf-find-l.sh`（libpdf と PDF Viewer の核、C89 -pedantic、plain と ASan+UBSan） | 両方 `52 passed, 0 failed`、`host-pdf-find-l: PASS` |
| `sh plan/ws128/tests/run-host-page-text.sh`・`run-host-pdfviewer-find.sh`（既存の回帰） | PASS・PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/pdfviewer build/p1-uat/bin/wayland build/p1-uat/dynamic/libpdf.so` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（変えた file） | 指摘 0（titlebar-shell.c 1830・1838 と menu.c 139 の既存の指摘は変えていない行） |

未実施: QEMU（T1）、指の実機（UAT）。
