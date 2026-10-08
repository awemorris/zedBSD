<!-- awesome-plan project=zedbsd record=ws190-p003 -->
# ws190-p003: Text Editor の指の選択の編集の bar

Status: cleared（2026-10-08 Q1: T1-443 1〜5 PASS（6 は OSK の無い image で not-run）、T1-445 1〜6・T1-449 の速い drag PASS）
Disposition: normal
Parent: [WS190](../ws.md)
Queue: q899（P1、2026-10-08）
Design: [p001](../phase001/phase.md) 第 2 版 §3。p002 の kl_text_bar・kl_text_touch の bar に依存（main に統合済み）

## 実装（commit、agent/p1）

- 3e9d4f7f4 と、その後の style の直し: `textedit.h` の te_app に `bar`・`bar_held`・`bar_logged`。`main.c` の `main_fingers` は、dialog の時は bar を消してから早く return する。text の region の後に `main_bar` で bar を置いて記録する（`kl_text_bar_buttons`・`_layout`・`_hit`。window から keyboard の inset を除いた範囲、id `MAIN_TEXT_BAR`）。`kl_ui_end` と `te_app_touch` の後に `main_bar_press` を呼ぶ:
  - Copy: hide_bar する。
  - Cut・Paste: `kl_text_touch_set_selection` で handle と bar を消す。
  - Select All: `kl_text_touch_select`。
- `main_frame` は handle の後に `kl_text_bar_draw` で bar を描く。log は `TOUCH bar shown buttons= rect=`・`TOUCH bar hidden`・`TOUCH bar press button=`。
- `app.c`: key の選択で touch を戻す条件を `handles || bar` にした（語の無い double tap の後の key でも bar が消える）。`KL_TEXT_TOUCH_BAR` が来たら描き直す。
- host 試験: `plan/tools/textedit/host-core.c` に `test_touch_bar` を足した（4 件。double tap の語と bar、key で消える、caret の bar が key で消える）。
- シナリオ: `tests/scenarios/apps/textedit/touch-select.md`（active）。

## 確認（2026-10-08、P1）

- `sh plan/tools/textedit/host-core.sh`: 62/62。
- build: amd64（config-amd64-aat、BUILD=build/ws190）の textedit と Linux の keiland all は warning 0。style-check の新しい指摘は 0。`check-scenarios.py` は PASS。
- QEMU は未実施（T1 に依頼）。

## T1-443・T1-445 の結果と速い drag の直し（2026-10-08、P1 q904 の前）

- T1-445（Q1 の要約）: 1〜6 は確認。3 の `aat touch-drag 345 129 443 150 --steps 10`（間を置かない速い drag）は `anchor=0 caret=5`（1 行目）で止まり 2 行目に届かない。touch-down → 150 ms ごとの move 5 回 → up なら `anchor=0 caret=18`。
- 原因（code を読んだ）: libkeiland の `kl_ui` は、指の選択を frame の始め（`ui_drag_step`、`kl_ui_begin`）でだけ指に追わせ、その位置は `kl_gesture_drag_offset` の resample（実の指より遅れる）だった。drag の終わり（`KL_GESTURE_DRAG_END`）は追わずに `kl_text_touch_drag_end` するので、frame の前に動いて離れた速い stroke は、最後の frame が見た途中の位置で終わる。move の取りこぼしではない（gesture は全ての move を受けている）。
- 直し: `userland/desktop/libkeiland/ui/ui.c` の `ui_gesture` の DRAG_END で、UI_DRAG_SELECT なら離した点（`gesture->x`・`y`、gesture の lift の点）へ `kl_text_touch_drag` してから終える。handle の drag（grip を引く）も同じ道。Text Editor・kl_field・kl_text_area の全てに効く。
- 試験: `plan/ws190/tests/host-touch-select.c` に `quick_drag`（1 ms 間隔の 10 回の move、frame 無し、すぐ up）と `test_quick_drag`（Select All の後、終わりの knob を 1 行目の `one` の後へ投げる → `anchor=0 caret=3`）。直しの前の ui.c では FAIL、直しの後 `sh plan/ws190/tests/run-host.sh` → host-touch-select 44/44、ws190-host PASS（plain・ASan/UBSan）。回帰: host-core 62/62、host-input 96/96、host-widgets 94/94、host-chooser 85/85。
- build（warning 0）: `make -j16 BUILD=build/ws190 ZEDBSD_CONFIG=plan/ws190/tests/config-amd64-aat-touch.mk build/ws190/dynamic/libkeiland.so build/ws190/bin/textedit build/ws190/bin/kuidemo`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws190-linux all`。style-check の新しい指摘 0（ui.c の 2 件は既存の行）、`git diff --check` 0。
- T1-443 の 4（下の handle は knob の位置を掴む）: 設計どおり（knob は文字の境の下、phase001 §handle の記録）。シナリオ `tests/scenarios/desktop/widgets/touch-select.md` の 4 に掴む位置を書いた（check-scenarios PASS）。
- 再試験（T1）: 同じ image の作り方（config `plan/ws190/tests/config-amd64-aat-touch.mk`、今の tree）で apps.textedit.touch-select 3 を `aat touch-drag 345 129 443 150 --steps 10`（間を置かない）で 2 回 → `TEXTEDIT TOUCH … anchor=0 caret=18`・`bar shown`。回帰に desktop.widgets.touch-select 2・4。
