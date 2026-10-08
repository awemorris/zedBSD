<!-- awesome-plan project=zedbsd record=ws190-p003 -->
# ws190-p003: Text Editor の指の選択の編集の bar

Status: test-wait（T1-445、未実行。2026-10-08 q902 P1 の照合: 実装・host 試験まで）（旧: in-progress（実装・host 試験は済み、T1 の AAT の依頼を Q1 へ））
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
