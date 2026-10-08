<!-- awesome-plan project=zedbsd record=ws177-p013 -->

# ws177-p013: libkeiland の field・text area の clipboard・語・undo（案 K の 9）

Parent: [WS177](../ws.md)
Status: test-wait（2026-10-08 P1 q887 の 3: 実装・host PASS・zedBSD と Linux の build warning 0。QEMU は T1 の AAT `apps.notes.text-box-follow`）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q887 の 3（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 9（WS175 ws175-p008）、[案](../phasing-20261008.md) の K、[ws177-p012](../phase012/phase.md) から分けた

## 設計と変更（2026-10-08 P1）

全ての app の kl_field・kl_text_area に効く（**KL_VERSION 67**）。

- 鍵: Ctrl+Z（undo）、Ctrl+Shift+Z と Ctrl+Y（redo）、Ctrl+C・Ctrl+X・Ctrl+V（copy・cut・paste）、Ctrl+Left・Ctrl+Right（語、Shift で選択）。`field_wants`・`area_wants` はこれらを取る（前は Ctrl は A だけ）。field を focus している間、これらの鍵は app に届かない。
- 履歴: `kl_ui` が最後に編集された text widget の履歴を持つ（`ui.c` の `struct ui_undo`、100 段、古い物から捨てる）。各入力（鍵 1 つ、input method の commit 1 つ、削除 1 つ、cut・paste 1 つ）の前後の text を比べ、両端の共通を除いた「取った bytes・入れた bytes・位置・前の caret と選択」を 1 段にする（`keiui_edit_record`）。履歴の終わりの text の FNV-1a hash を持ち、app が `kl_field_set` などで text を変えたら履歴は使わない（undo で app の text を壊さない）。別の widget の編集で履歴は新しくなる。undo は前の caret・選択に戻し、redo は入れた後に caret を置く。undo の後の新しい変更は redo を捨てる。
- clipboard: `kl_ui_window_text(ui, window)`（毎 frame 呼ばれる、引数の const を外した）が input を窓に結び、`kl_window_copy`・`kl_window_paste` を関数の pointer で持つ（`keiui_ui_set_window`。ui.c が window の code に link しないので host の試験はそのまま組める）。窓に結ばれていない kl_ui では clipboard は何もしない。secret の field は copy・cut しない。field の paste は 255 byte まで（input method の commit と同じ口）、text area は 255 byte ずつに分けて 8192 byte まで。
- 語: 英数字・下線・ASCII 外の byte を語とし、その境で止まる（UTF-8 の文字の中には止まらない）。
- Notes: menu の Undo・Redo（System Menu の Ctrl+Z・Ctrl+Shift+Z）は box が開いている時、box の widget に Ctrl+Z・Ctrl+Shift+Z を渡す（`notes_box_key`）。前の「開いた時の文字に戻す」（`notes_box_revert`）は消した。

- 変更: `userland/desktop/libkeiland/ui/ui.c`・`internal.h`・`field.c`・`text-area.c`・`text-input.c`、`userland/desktop/include/keiland/keiland.h`（KL_VERSION 67、`kl_ui_window_text` の const）、`userland/desktop/notes/main.c`・`box.c`・`app.h`。
- 試験: `plan/ws177/tests/host-text-edit.{c,sh}`（新）。`plan/ws177/tests/host-field-limit.c` に新しい関数の stub。AAT `apps.notes.text-box-follow` に box の中の Ctrl+Z・Ctrl+Shift+Z の段（bytes 49 → 48 → 49）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-text-edit.sh` → PASS（plain と ASan/UBSan、22 の check）: 語の移動、Shift の選択、undo 2 回・redo（Ctrl+Shift+Z・Ctrl+Y）、redo の無い時、undo の後の打鍵で redo が消える、Ctrl+A・C、Ctrl+X、Ctrl+V、paste と cut をそれぞれ 1 段で戻す（cut の戻しで選択も戻る）、`kl_field_set` の後の Ctrl+Z は何もしない、secret の field の Ctrl+C・X、text area の改行を含む undo、2000 byte の paste と 1 段の戻し、text area の語。
- 回帰: `plan/ws090/tests/host-input.sh`（96/96）、`plan/ws090/tests/host-widgets.sh`（94/94）、`plan/ws177/tests/host-field-limit.sh`（PASS）、`plan/tools/textedit/host-core.sh`（58/58）。
- build（warning 0）: zedBSD `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=plan/ws089/tests/config-amd64-settings.mk build/p1-k/dynamic/libkeiland.so build/p1-k/bin/notes build/p1-k/bin/settings build/p1-k/bin/files build/p1-k/bin/textedit`、Linux `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-linux all`。style-check: ui.c・text-area.c・field.c は前と同じ件数（既存の物だけ）。`check-scenarios.py` PASS。

## 未実施

- QEMU（T1）: AAT `apps.notes.text-box-follow`（box の中の undo・redo）。他の app（Settings・Files・file chooser）の field の Ctrl+C・V を QEMU で見ていない（host の試験だけ）。
- 打鍵ごとの undo は打鍵をまとめない（1 語ずつではない）。

## Event

2026-10-08 / q887-i03（P1）: 実装と host・build の確認。
