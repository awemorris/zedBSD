---
id: desktop.dnd.content-to-desktop
title: 画像と文字を desktop に落とすと、Image.png と Text Clipping.txt ができて落とした所に並ぶ
status: active
areas: [compositor, dnd, files, desktop]
paths: [userland/desktop/files/dnd.c, userland/desktop/files/ui-desktop-drag.c, userland/desktop/files/main.c, userland/desktop/files/window.c]
machine: either
human: look
since: ws189-p003
---

## 目的
desktop に落とすと内容の file ができる（WS189 の決定）: 画像は PNG の file、文字は text の file になり、drop の cell に置かれること。名前が重なれば「 2」が付くこと。

## 準備
desktop（Files の desktop が動いている）。`~/Desktop` に Image.png・Text Clipping.txt が無い。Notes に画像が 1 つある頁（desktop.dnd.photo-to-notes の後、または Insert Image）と、
Text Editor に `desk clip` を打った窓。desktop の空いた所が見えるように窓を置く。

## 操作と確認
1. 操作: Text Editor の `desk clip` を選び、その上で押して desktop の空いた所へ動かし、止めて撮り、離す。`aat mark start` は押す前。
   確認事項: 文字の file。正解: `ZFILES DESKTOP drop enter self=0 files=0 content=1`、`KWL DATA drag state=copy`、`ZFILES DND content path=/home/kei/Desktop/Text Clipping.txt bytes=9 type=1`、
   `ZFILES DESKTOP drop-file path=...`、`ZFILES DESKTOP dropped name=Text Clipping.txt column=C row=R`、file の中身が `desk clip`。desktop の離した所に icon。確認方法: log、`cat`、撮影。
2. 操作: Notes の画像を Select で押して窓の外へ出し（desktop.dnd.photo-to-notes の 3）、desktop の別の空いた所で離す。
   確認事項: 画像の file。正解: `ZFILES DND content path=/home/kei/Desktop/Image.png ... type=4`、file の先頭が PNG の signature（`head -c 8 | od -c`）、desktop に icon。確認方法: log、od、撮影。
3. 操作: 1 をもう一度。
   確認事項: 重なる名前。正解: `Text Clipping 2.txt`。確認方法: log、ls。

## 合格
3 つの file、名前、置かれた cell。

## 注記
folder の item の上の画像・文字は断る（印は不可）。folder の窓（desktop でない Files）は今は画像・文字を受けない（Future Work）。
