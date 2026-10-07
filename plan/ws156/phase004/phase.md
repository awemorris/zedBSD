<!-- awesome-plan project=zedbsd record=ws156-p004 -->

# ws156-p004: 通知の log（Super+N、ring、すべて消去、log の ×）

Phase ID: `ws156-p004`
Parent: [WS156](../ws.md)
Status: cleared（2026-10-08 Q1: T1-375b QEMU PASS、20 項目 ok、成果物は T1 の worktree build/t1-375b/OUT/）
Phase disposition: normal

## 範囲

[p001](../phase001/phase.md) §5: Super+N で popup と同じ位置・大きさの板に log を出す（動かない、150 ms の fade-in）、ring の左右、すべて消去、log の ×、閉じ方（Super+N・Esc・板の外の click・10 秒の無操作）。保存は session の memory だけ（p002 の model、100 個）。

## 実装

- `userland/desktop/wayland/notify-log.c`（新）: ring は「最新 → … → 一番古い → すべて消去の板 → 最新」（→ で古い方へ、← で新しい方へ。最新から ← で「Clear all notifications」の板。p001 §5 の「最新の次にすべて消去」「端の次は最新」を一つの ring にした読み）。開いた時は最新。板の左右の外に矢印（click で移る）、card の × でその通知を log から消す（dismiss、残さない）、「Clear all notifications」の button と、その板での Enter で log を空に（各 client に closed(CLEARED)）。log が空なら「No notifications」の板だけ。log の行 `KWL NOTIFY log open|at|close|clear|dismiss`。
- `notify-popup.c`: 描画を `kwl_notify_draw_card`・`kwl_notify_draw_glass` に分け（popup と log が共用）、`kwl_notify_board_width`・`kwl_notify_board_top`・`kwl_notify_card_close_at`。log の tick・描画・button は popup の tick・描画・button から呼ぶ（shell.c への差し込みを増やさない）。`kwl_notify_popup_showing` は log の板が開いている間も 1。
- `notify-view.h`（新）: popup と log の共用の宣言。
- `notify-shell.c`: `kwl_notify_clear_log`。
- `shell.c`: `kwl_glass_key` に 3 行（volume の key の後）。`kwl.h` 宣言 2 行、Makefile 3 本、`ja/wayland.tr` に 2 行（No notifications・Clear all notifications）。

## 確認

| コマンド | 結果 |
| --- | --- |
| zedBSD `make … build/p2-k/bin/wayland`、Linux `make -f userland/desktop/keiland-linux.mk … all` | warning 0 |
| `python3 plan/tools/style-check.py`（notify-log.c・notify-popup.c・notify-shell.c・notify-view.h） | 新しい違反 0 |
| `python3 tools/i18n/tr.py check …/ja/wayland.tr …` | 147/147 0 problems |
| `sh plan/ws131/tests/host-system.sh`、`sh plan/ws156/tests/run-host-notify-flow.sh`、`run-host-notify-model.sh` | PASS |

未実施: ring の移り・消去・× の host の試験（log の module は server と描画に依るので、QEMU の試験で確かめる）。QEMU（Super+N、←・→、すべて消去、×、Esc、外の click、10 秒）は p005 で T1。

## 残り

- p005: 全文の規約、T1 の QEMU（p003・p004 をまとめて）、実機の UAT。
