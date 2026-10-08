<!-- awesome-plan project=zedbsd record=ws177-p043 -->

# ws177-p043: titlebar の無い時の窓の中の検索の欄、Enter の後の caret（案 L の 4）

Parent: [WS177](../ws.md)
Status: test-wait 予定（2026-10-08 夜 P1 q907: 実装・host PASS・build warning 0。T1 の依頼を Q1 へ）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q907-i01（P1、承認は p040 と同じ）
Origin: [backlog-p2](../backlog-p2.md) 23・41 行（WS128 ws128-p004、q826）、[案](../phasing-20261008.md) の L。

## 範囲と設計

- 23: compositor に titlebar が無い（`pv_titlebar.shown` が 0）時、Ctrl+F・Edit > Find は窓の右上に検索の欄（libkeiland の `kl_field`、
  kl_ui で frame の上に描く。IME・OSK・undo・clipboard は kl_field の物）を開く。打つたびに `pv_find_text`、Enter は次、Shift+Enter は前、
  Esc は欄を閉じて印を消す。欄の横に一致の数（p041 の文）。欄を開いている間、pointer の press が欄の外なら欄は keyboard を手放す
  （欄は残る）。核（開閉・矩形・文字の受け渡し）は view.c/find.c、kl_ui の描画と入力は main.c。
- 41: Enter の後に field へ戻した時、caret を末尾に置き選択しない。compositor（`wayland/titlebar-shell.c` の `shell_focus`）は search の
  control を `KL_FOCUS_EDIT` で focus された時は anchor を末尾にする（`KL_FOCUS_FIELD` は今のまま全体を選ぶ: Ctrl+F で打ち直せる）。
  PDF Viewer は Enter の後に `kl_window_focus_control_mode(..., KL_FOCUS_EDIT)` を使う。

## 確認

- host: `plan/ws177/tests/host-pdf-find-l.sh` の bar の群（核の開閉・文字・Enter・Esc）。compositor は build。
- QEMU（T1）: AAT（titlebar の有る desktop で、Enter の後に続けて打つと query に足される: `KWL TITLEBAR focus … edit=1` と
  FIND の log）と、titlebar の無い時の欄（`ZEDBSD` の System Menu の無い compositor の構成が要る。無ければ欄の host の試験だけ）。
- build warning 0、style-check。

## 実装と確認（2026-10-08 夜、P1 q907）

- 23: `userland/desktop/pdfviewer/bar.c`（新）: kl_ui と kl_field の panel（`pv_bar_open`・`pv_bar_close`・`pv_bar_focus`・`pv_bar_input`・`pv_bar_draw`）。
  変化で `pv_find_text`、Enter（kl_field の SUBMITTED）で次、Shift+Enter で前、Esc（CANCELLED）で閉じる、欄の右に `pv_find_status`。
  panel の外の press は keyboard を pages に返す。核は `find.c` の `pv_find_bar_open`・`pv_find_bar_close`・`pv_find_bar_place`（`viewer.h` の
  `struct pv_bar_place`・`PV_BAR_*`）。`main.c`: Ctrl+F で titlebar が無ければ欄を開く、入力を先に欄へ、frame の後に欄を描く。pages の Esc も欄を閉じる。
  Makefile（target・Linux・FreeBSD）に bar.c。
- 41: compositor `wayland/titlebar-shell.c` の `shell_focus`: search を `KL_FOCUS_EDIT` で focus した時は選択なし・caret を末尾。PDF Viewer の
  `titlebar.c` は Enter の後に `kl_window_focus_control_mode(…, KL_FOCUS_EDIT)`。`keiland.h` の KL_FOCUS_* の説明。
- AAT: `tests/scenarios/apps/pdfviewer/find-select.md` に 3a（Enter の後に `s` を打つと「lines」）、`plan/tools/aat/scenarios/helpers_pdfviewer_find.py` に同じ手順。
- 試験: host-pdf-find-l の bar の群（Ctrl+F の依頼、開く、右上の place、数の文、狭い窓、Esc で閉じる）。

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-pdf-find-l.sh`（libpdf と PDF Viewer の核、C89 -pedantic、plain と ASan+UBSan） | 両方 `52 passed, 0 failed`、`host-pdf-find-l: PASS` |
| `sh plan/ws128/tests/run-host-page-text.sh`・`run-host-pdfviewer-find.sh`（既存の回帰） | PASS・PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/pdfviewer build/p1-uat/bin/wayland build/p1-uat/dynamic/libpdf.so` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（変えた file） | 指摘 0（titlebar-shell.c 1830・1838 と menu.c 139 の既存の指摘は変えていない行） |

未実施: QEMU（T1: AAT の find-select の 3a、titlebar の無い compositor の構成での欄は、その構成が AAT に無ければ未実施のまま）。
