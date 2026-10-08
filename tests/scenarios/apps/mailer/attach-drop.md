---
id: apps.mailer.attach-drop
title: Mail の作成で file を Attach... と drop で添付し、外し、multipart/mixed で送る
status: active
areas: [mailer, dnd]
paths: [userland/desktop/mailer/compose.c, userland/desktop/mailer/view.c, userland/desktop/mailer/main.c, userland/desktop/mailer/mail.h, plan/tools/mail/fake-mail-server.py]
machine: either
human: look
since: ws189-p004
---

## 目的
ws189-p004（ユーザーの決定「WS189 で添付も作る」）: 作成の pane の Attach... と、Files・Photos からの drop で添付し、chip の x で外し、送った message が
multipart/mixed（本文と base64 の添付）になることを確かめる。

## 準備
`apps.mailer.read-compose` の準備と手順 1〜2（偽の server、account を足す）。試験の image は `plan/ws189/tests/config-amd64-aat-dnd.mk`。runner の試料
`/tmp/aat-samples/sample.png` と `sample.pdf`。kei で `mkdir -p ~/attach && cp /tmp/aat-samples/sample.pdf ~/attach/report.pdf && echo attached > ~/attach/note.txt`。
Files を `files ~/attach` で開き、Photos を開いて `sample.png` が grid にあること（Photos の試料の場所は `apps.photos` のシナリオと同じ）。窓は重ならないように置く。

## 操作と確認
1. 操作: Mail で Ctrl+N。作成の pane の下の行を撮り、Attach... を click、chooser で `~/attach/note.txt` を選んで開く。`aat mark start` は Ctrl+N の前。
   確認事項: Attach...。正解: 下の行に Attach... の button、click で `MAIL ATTACH asked`・`MAIL ATTACH chooser ok=1`、選ぶと `MAIL ATTACH add name=note.txt type=text/plain bytes=9 count=1`、
   chip「note.txt」と大きさ。確認方法: log、撮影。
2. 操作: Files の `report.pdf` を押して Mail の作成の pane の上へ動かし、止めて撮り、離す。
   確認事項: file の drop。正解: 止めた間 `KWL DATA drag state=copy`、添付の行が枠で光る。離すと `MAIL ATTACH add name=report.pdf type=application/pdf … count=2`、
   `MAIL DND drop type=2 added=1 count=2`、`KWL DATA drag finish`、Files の `ZFILES DND end dropped=1`（file は Files に残る）。確認方法: log、撮影。
3. 操作: Photos の grid の 1 枚目を Mail の作成の pane へ drag して離す。
   確認事項: 画像の drop。正解: `MAIL ATTACH add name=image.png type=image/png … count=3`、`MAIL DND drop type=4 added=1 count=3`、chip「image.png」。確認方法: log、撮影。
4. 操作: chip「note.txt」の x を click。
   確認事項: 外す。正解: `MAIL ATTACH remove index=0 count=2`、chip が 2 つ。確認方法: log、撮影。
5. 操作: To に `ben@example.com`、Subject に `Attachments from AAT`、本文に `Two files.`、Send。
   確認事項: 送信。正解: `MAIL SENT account=0`、添付の行が空に戻る。server の最新の `smtp-N.eml` が `Content-Type: multipart/mixed; boundary=` で、本文の part に
   `Two files.`、`filename="report.pdf"` と `filename="image.png"` の part（`Content-Transfer-Encoding: base64`）。report.pdf の part を base64 から戻すと
   `sample.pdf` と同じ（SHA-256）、image.png は PNG の署名で始まる。確認方法: log、host の server の file。
6. 操作: Mail の受信箱の一覧を出したまま（作成していない）、Files の `report.pdf` を Mail の窓の上へ drag して離す。
   確認事項: 作成していない時は受けない。正解: `KWL DATA drag state=refused`、`MAIL ATTACH add` が増えない。確認方法: log。

## 合格
1〜6 の正解。見えは撮影（添付の行、drop の枠、chip）。

## 注記
- 添付の上限: 16 個、合計 25 MiB（越えると「No more files fit in this message.」）。host 試験 `plan/ws189/tests/run-host-mail-attach.sh` が RFC 2231 の日本語の名前と上限を確かめる。
- 後片付け: `rm -r ~/attach`（guest の中）。
