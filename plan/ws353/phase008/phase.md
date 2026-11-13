<!-- awesome-plan project=zedbsd record=ws353-p008 -->

# ws353-p008: animation・frame の pacing・時計、main の poll を -1 に（main loop の 4）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.2・§5.9、対応表の 1-01・2-13・2-17・2-38。

## 目的

animation を「動いている間だけ frame の完了で一歩」にし、frame の pacing と時計を timer にする。残りの期限を timer に移し終えたら、main の poll の timeout を常に -1 にし、`kwl_due`（branch を merge した時）と毎 pass の tick を消す。

## 対象（file）

- animation の 16 か所（apps-bar.c・greeter.c・keyboard.c・arrange-shell.c・corner.c・home.c・notify-log.c・power-dialog.c・notify-popup.c・touch.c・shell.c・sleep.c）: 描く物は「描いた frame の完了で一歩進め、まだ動いていれば次を描く」。完了は render の COMPLETED（p011 の後は REAPED。どちらも GPU の完了の直後）で、p010 の後は T17（shown）の output の SHOWN。描かずに時刻だけで進む物（sleep の保留、spring の判断）は timer。
- frame の pacing（display.c:255 `frame_done + frame_wait`）: timer。
- 時計（main.c の次の分、shell.c・greeter.c）: 時計が見える時だけ arm、消灯・時計の面が無い間は disarm。実時刻の次の分までの長さを monotonic に直して arm し、handler で実時刻を読み直して次を arm する（時刻の設定の後のずれを次の発火で直す。設計 §5.5、レビュー M7）。
- main.c: poll の timeout を -1 に固定。`kwl_due`・`kwl_due_soon` と `due_ms` を消す（branch を merge した時）。`kwl_backend_wakes()` が 0 の backend（Linux・FreeBSD）の 10 ms は、その backend の事象の口が出来るまで残してよい（keiland-linux・freebsd は崩れてよい platform）。

## 残す振る舞い

- animation の速さ（時刻で位置を決める物は時刻で、frame ごとの物は frame で）、pacing の待ちの長さ、時計の表示。

## 確かめ

- build warning 0。QEMU: slide・fade・OSK・touch の追従・notify の popup が前と同じ時間で終わる（`--log-frames` の時刻を file から）。guest の時刻を進めた・戻した後に時計の分が次の発火で合う。idle 60 s の main の `KWL PERF` の `timeouts` が 0、WS352 の poll の診断で main の timeout の起床が 0（時計が見える時は分に 1 回）。

## 受け入れ

- main の poll が -1 で、毎 pass に全部の tick を呼ぶ code が無い。結果・SHA・未実施を書く。

## 依存

- p006・p007。
