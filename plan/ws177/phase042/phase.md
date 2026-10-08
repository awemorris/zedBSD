<!-- awesome-plan project=zedbsd record=ws177-p042 -->

# ws177-p042: PDF Viewer の選択（頁をまたぐ・語と行・全て・指・回転した字の塗り）（案 L の 3）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P1 q907 に立てて着手）
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
