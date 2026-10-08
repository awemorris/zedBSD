<!-- awesome-plan project=zedbsd record=ws177-p041 -->

# ws177-p041: PDF Viewer の検索の一致の規則・数・速さ・読めない字（案 L の 2）

Parent: [WS177](../ws.md)
Status: test-wait（T1-474、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q907-i01（P1、承認は p040 と同じ）
Origin: [backlog-p2](../backlog-p2.md) 18・19・22・25 行（WS128 ws128-p004）、[案](../phasing-20261008.md) の L。

## 範囲

- 18: 行をまたぐ語・ハイフンで切れた語・空白の数の違い。
- 19: ASCII 以外の大文字・小文字、全角・半角、濁点の合成、合字。
- 22: 大きな文書の検索の速さと一致の数の表示。
- 25: ToUnicode の無い・壊れた font の文字（U+FFFD）を検索・copy で知らせる。

## 設計

### 頁の検索の鍵（find.c、頁ごとに 1 度作って `pv_page` に置く。document.c が text と一緒に放す）

頁の文字（`pdf_page_text`）から鍵の列を作る。鍵は 1 つの code point と、元の文字の範囲（`origin`〜`last`）と「飛ばしてよい」の印。
- 正規化（`find_fold`、query にも同じ物を使う）:
  - 空白（U+0020・TAB・U+00A0・U+2000〜200A・U+202F・U+205F・U+3000）は U+0020。続く空白は 1 つの鍵にまとめる。
  - U+00AD（soft hyphen）は鍵にしない。
  - 全角の ASCII（U+FF01〜FF5E）は ASCII、半角カナ（U+FF61〜FF9F）は全角カナ（半角の濁点・半濁点 U+FF9E・FF9F は結合の U+3099・309A）。
  - 結合の濁点・半濁点（U+3099・309A）と、単独の ゛゜（U+309B・309C）は、前の鍵が合成できる仮名なら合成（か→が、は→ぱ、う→ゔ、
    ワ→ヷ など。Unicode の合成の表の仮名の分）。合成した鍵の範囲は濁点の文字まで伸ばす。
  - 合字 U+FB00〜FB06 は ff・fi・fl・ffi・ffl・st（鍵は複数、範囲は同じ 1 文字）。ß は ss。
  - 大文字・小文字（simple case folding の表の範囲を限る）: ASCII、Latin-1（U+00C0〜00DE、× を除く）、Latin Extended-A（U+0100〜017F の
    組、İ→i、ſ→s、Ÿ→ÿ）、ギリシャ（U+0391〜03A9、ς→σ、アクセント付き 0386・0388〜038A・038C・038E・038F）、キリル（U+0400〜042F）。
- 行の終わり（`PDF_TEXT_LINE_END`）の後に「飛ばしてよい」空白の鍵を足す（既に空白なら足さない）。行の終わりの文字が `-`・U+2010・
  U+2011 なら、その鍵も「飛ばしてよい」にする。こうして「inter-⏎national」は「international」にも「inter-national」にも一致し、
  日本語の行の折り返し（空白の無い行の境）は空白なしの query に一致する。
- 一致（`find_match`）: 鍵 k と query の j で、同じなら両方進む。違って鍵が「飛ばしてよい」なら鍵だけ進む。他は不一致。最初の鍵は
  query の最初と同じでなければならない（飛ばす鍵からは始めない）。一致の範囲は最初の鍵の `origin` から最後に使った鍵の `last` まで。
- query は同じ正規化で鍵の列にし（範囲は要らない）、先頭と末尾の空白は落とさない（打っている途中の語の区切り）。

### 一致の数と背景の読み（22）

- 頁ごとに一致の数を数えて置く（`pv_page.find_count`、`find_generation` が今の query の世代の時に有効）。query が変わると世代を
  進める。
- `pv_app_tick` から `pv_find_tick`: 数えていない頁を、表示の頁から順に、1 回の tick で 8 ms（`pv_clock` で測る、少なくとも 1 頁）まで
  読んで数える（頁の文字の読みが重い文書でも打鍵と描画を止めない）。全ての頁を数え終えたら `FIND count query=… total=N` を log。
  まだ数え終えていない間は tick を 1 ms 後にまた求める。
- 表示: 見つけた所へ動くたびに「Match k of N」（全ての頁を数え終え、k が分かる時）、数え終える前は「Match on page P」を
  2 秒の知らせで出す。窓の中の検索の欄（p043）は同じ文を欄の横に常に出す。
- `pv_find_next` は今のまま同期で頁を読む（数える tick が先に読んだ頁は読まない）。

### 読めない字（25）

- 一致がない時、文書の読んだ頁に U+FFFD があれば「Not found (some text could not be read)」と出す（今は「Not found」）。
- copy（`pv_select_copy`）: U+FFFD を含む時は copy はそのまま（字の数を変えない）、知らせ「N characters could not be read」と
  log `COPY unreadable=N`。

## 確認

- host: `plan/ws177/tests/host-pdf-find-l.sh` の find の群（python で作る PDF: 行をまたぐ語・ハイフンの語・空白の多い行・全角と半角・
  濁点の結合と半角カナ・ギリシャとキリルの大文字・合字の glyph（ToUnicode で U+FB01）・ToUnicode の無い font）。各 query の
  一致の頁・範囲・数、tick で数え終える、知らせの文。既存の `plan/ws128/tests/run-host-pdfviewer-find.sh`。
- build warning 0、style-check。QEMU（T1）は p043 の後にまとめて。

## 実装と確認（2026-10-08 夜、P1 q907）

- `userland/desktop/pdfviewer/find.c`: 頁の鍵（`struct pv_find_key`、`find_key_make`・`find_key_add`・`find_fold`・`find_case`・`find_compose`）、
  鍵での一致（`find_match`、飛ばしてよい鍵）、query の鍵（`find_query_keys`）。`pv_find_tick`（`pv_app_tick` から、8 ms ずつ頁を読んで数える）、
  `pv_find_status`（「Match k of N」、数え終える前は「Match on page P」）、見つけた時の知らせ、見つからない時に読めない字があれば
  「Not found (some text could not be read)」、copy の U+FFFD の数の知らせと log の `unreadable=`。`viewer.h` の `pv_page`（`find_key`・
  `find_count`・`find_generation`）と `pv_app`（鍵・数の状態）、`document.c` が鍵を放す。
- log の形は前と同じ（`FIND found query=… page= from= length=`。COPY の行の末尾に `unreadable=` を足しただけ）で、AAT の
  apps.pdfviewer.find-select の regex はそのまま通る。
- 試験: host-pdf-find-l の find の群（13 の規則の query、「が」の 2 つ、見つからない時の文、32 の一致を tick で数え「Match 1 of 32」・F3 で
  「Match 2 of 32」、全てを copy して「3 characters could not be read」）。

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-pdf-find-l.sh`（libpdf と PDF Viewer の核、C89 -pedantic、plain と ASan+UBSan） | 両方 `52 passed, 0 failed`、`host-pdf-find-l: PASS` |
| `sh plan/ws128/tests/run-host-page-text.sh`・`run-host-pdfviewer-find.sh`（既存の回帰） | PASS・PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/pdfviewer build/p1-uat/bin/wayland build/p1-uat/dynamic/libpdf.so` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（変えた file） | 指摘 0（titlebar-shell.c 1830・1838 と menu.c 139 の既存の指摘は変えていない行） |

制限: 大文字・小文字は Latin-1・Latin Extended-A・ギリシャ・キリルの simple case folding の範囲。アクセントを外した一致（café と cafe）はしない。
`pv_find_next` は数える tick が読んでいない頁を同期で読む（巨大な文書の最初の検索は今と同じ速さ）。未実施: QEMU（T1）。
