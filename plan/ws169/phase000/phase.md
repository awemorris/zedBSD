<!-- awesome-plan project=zedbsd record=ws169-p000 -->

# ws169-p000: メーラの app の UI の mock

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-181 の撮影の後、p002〜p005 で実装・cleared）（旧: in-progress（実装・host の PNG・build は済み。QEMU は T1 待ち））
Disposition: normal
Parent: [WS169](../ws.md)
Queue: q744（2026-10-05、P2）
依存: なし

## 範囲（Q1）

メーラの app の外側だけ。account・folder の一覧、メールの一覧、読む画面、書く画面（送信は「backend が無い」）。data は固定の試験 data。API・backend は作らない。

## 実装（2026-10-05、P2）

- `userland/desktop/mailer/`（新規、package `mailer`、既定では image に入らない `n`、窓の題は「Mail」。POSIX の `mail` と名前がぶつからないように binary は `mailer`）:
  - `mailer.h`・`data.c`: 2 つの account（Personal・Work）、folder（Inbox・Sent・Drafts・Archive・Trash）、12 通の試験のメール（未読・添付・日本語・送信済み・下書き、サインインの code を含む 1 通）。
  - `view.c`: 3 つの pane。左に「Mail」・New Message・account ごとの folder（未読の数）・下に Get Mail と「Not connected (no backend yet)」。中に folder の名前と数・検索（差出人・件名・本文）・メールの行（未読の点、差出人、日付、件名、本文の 1 行、添付の印）。右に Reply・Reply All・Forward・Archive・Delete の帯、件名、差出人（頭文字の丸・名前・address・宛先・日付）、サインインの code を目立たせる帯（「Apps you allow will be able to fill it in (later).」、WS169 の目標の browser の自動入力への伏線）、本文、添付の card。New Message・Reply・Reply All・Forward で右の pane が書く画面（To・Cc・Subject の欄、本文は click で keyboard を受け Enter で改行・Backspace、返信は宛先と件名と引用を入れる）。幅 900 未満では folder を省き、一覧かメールの一方と戻る button。glass では pane が浮いた card。
  - Send・Get Mail・Archive・Delete は下に「No mail backend: …」の chip を 4 秒出し、`MAIL NOBACKEND action=…` を log に書く。開いたメールは動いている間だけ既読。
  - `main.c`: Phone と同じ kl_app の loop と glass（menu: File の New Message（Ctrl+N）・Get Mail・Quit Mail（Ctrl+Q）、Message の Send・Reply・Reply All・Forward・Archive・Delete）。
- `platform/amd64/vmunix.mk`: `bin/mailer` の link の規則。`userland/desktop/wayland/apps.conf`: App Home に「Mail」。
- 試験の image の config: `plan/ws169/tests/config-amd64-mailer.mk`（CI の image ＋ mailer）。

## 確かめ

- host: `sh plan/ws169/tests/run-host-mailer.sh` → PASS 10（start・open・reply・new・send（`NOBACKEND action=send to=3 subject=0 body=5`）・get・folder（Work の Inbox）・work・search（"code" で銀行の 1 通）・narrow-open）。PNG は `build/ws169/host-mailer-{start,attachment,reply,send,work,search,narrow-list,narrow-message,glass}.png`（目で見た。glass は壁紙をぼかした近似の地）。
- build: `make ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk BUILD=build/ws169-zed build/ws169-zed/bin/mailer` が warning 0。host の gcc `-Werror` も 0。style-check 0。
- 未実施: QEMU、実機、本文の日本語の入力（ASCII の key だけ）、添付・下書きの保存。

## q826-i02（2026-10-06 P2）: 一覧の添付の印

- T1-275 の `mailer.png` で、「Photos from the river walk」の行の右の小さな印が豆腐の四角に見えた。字ではなく、`kl_icon_file`（頁の絵の icon）を 16 px に縮めた物だった。
- 修正: `mailer/view.c` で、線で描く紙の clip（`view_clip`: 縦長の輪と内側の線、14 px）に替えた。
- 確認: `plan/ws169/tests/run-host-mailer.sh` は PASS（絵 `build/p2-mailer/host-mailer-start.png`）。Mail の zedBSD の build は warning 0。

