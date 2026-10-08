<!-- awesome-plan project=zedbsd record=ws177-p043 -->

# ws177-p043: titlebar の無い時の窓の中の検索の欄、Enter の後の caret（案 L の 4）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P1 q907 に立てて着手）
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
