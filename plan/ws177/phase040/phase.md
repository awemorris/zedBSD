<!-- awesome-plan project=zedbsd record=ws177-p040 -->

# ws177-p040: libpdf の頁の文字に form XObject の中の文字を入れる（案 L の 1）

Parent: [WS177](../ws.md)
Status: test-wait（T1-474、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q907-i01（P1、承認: Q1 の投入「WS177 案 L: PDF Viewer の検索と選択（plan/ws177/phasing-20261008.md の L、backlog-p2 17〜25・41、約 14.5 LW）」）
Origin: [backlog-p2](../backlog-p2.md) 17 行（WS128 ws128-p004）、[案](../phasing-20261008.md) の L。

## 範囲

- `pdf_page_text_open`（libpdf `editor.c`）の文字に、頁の content が `Do` で描く form XObject の中で示された文字を足す（入れ子の form も。PDF_CONTENT_FORMS_MAX まで）。
  Type 3 の glyph の手続きの中の文字は入れない（glyph の絵で、文字ではない）。
- 頁の行の後に、form の文字を「示された順」で足す。行の区切り: 別の form（別の `Do`）に移る時、または前の文字と基準線が字の高さの半分より
  離れる時。同じ行で前の文字の終わりから字の大きさの 1/5 より離れて始まる文字の前には空白（頁の行と同じ規則）。
- editor（Notes の編集）の scan の結果（shows・objects）は変えない: form の文字は `struct pdf_scan` の別の配列に置き、editor は読まない。

範囲の外（Q1 に報告）: **注釈（/Annots）の文字**。libpdf は注釈を描かない（appearance stream を走らせない。`grep Annots` で 0 件）ので、
検索で見つけても頁に見えない所を塗ることになる。注釈を描くようになった時に同じ形で足す（backlog-p2 17 行の注釈の分は残す）。

## 設計

- `content.c`: `struct content_run` に `text_forms`（run_form が入れた form の深さ。Type 3 の glyph は入れない）と `form_serial`（`Do` ごとに
  増やす）。`show_string` の各 code で、`scan_here` でなく `run->scan != NULL` かつ `form_depth > 0` かつ `form_depth == text_forms`
  （全ての段が form）の時に `scan_form_code` が文字と四隅（`scan_code` と同じ式）を `scan->form_characters`・`form_quads`・`form_breaks`
  に足す。区切りは足す時に決める（上の規則。`form_breaks`: 0 続き、1 前に空白、2 前で行が終わる）。
- `editor.c` `pdf_page_text_open`: 数える時に form の文字（と空白の分）を足し、頁の行の後に並べる。form の最初の文字の前で行を終える。
- `pdf.h` の説明（「Text inside a form XObject is not read」）を改める。

## 確認

- host: `plan/ws177/tests/host-pdf-find-l.sh`（新）の libpdf の群: form の中に文字のある PDF（python で作る: 頁の文字 1 行と、`Do` で
  2 つの form（1 つは入れ子、1 つは /Matrix で回した物）と、Type 3 の font の文字）で、form の文字が頁の行の後に出る・入れ子も・
  四隅が /Matrix の通り・Type 3 の glyph の中の文字が出ない・editor の shows の数が変わらない。既存の `plan/ws128/tests/run-host-page-text.sh`。
- build warning 0（libpdf は C89）、style-check。

## 実装と確認（2026-10-08 夜、P1 q907）

- `userland/base/libpdf/content.c`: `content_run` に `text_forms`・`form_serial`・`form_noted_serial`。`run_form` の出入りで数える。`show_string` の各 code で
  `scan_form_here`（scan があり、全ての段が form）なら `scan_form_code` が文字・四隅（`scan_glyph_quad`、`scan_code` と共有に切り出した）・前の区切り
  （`scan_form_break`: 最初・別の form の続き・基準線から字の高さの半分より離れる・字の高さより後ろへ戻る → 行、字の大きさの 1/5 より先 → 空白）を
  `pdf_scan` の新しい配列（`form_characters`・`form_quads`・`form_breaks`、`internal.h`）に足す。editor の shows・objects は変わらない。
- `userland/base/libpdf/editor.c`: `pdf_page_text_open` が頁の行の後に `editor_form_text` で form の文字を足す（区切りの空白は四隅が隙間）。`include/libc/pdf.h` の説明。
- 試験: `plan/ws177/tests/make-find-l.py`（forms.pdf: 頁の行、入れ子の form、30 度回した form、Type 3 の glyph の手続きの中の文字）、
  `host-pdf-find-l.c` の forms の群（行の並び「Page line|a|Form one|Nested two|after nested a|Turned text|」、glyph の中の「zz」が無い、回した T の
  基準線が 30 度）。

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-pdf-find-l.sh`（libpdf と PDF Viewer の核、C89 -pedantic、plain と ASan+UBSan） | 両方 `52 passed, 0 failed`、`host-pdf-find-l: PASS` |
| `sh plan/ws128/tests/run-host-page-text.sh`・`run-host-pdfviewer-find.sh`（既存の回帰） | PASS・PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/pdfviewer build/p1-uat/bin/wayland build/p1-uat/dynamic/libpdf.so` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（変えた file） | 指摘 0（titlebar-shell.c 1830・1838 と menu.c 139 の既存の指摘は変えていない行） |

注釈（/Annots）の文字は範囲の外（libpdf は注釈を描かない。Q1 に報告済み）。未実施: QEMU（T1）。
