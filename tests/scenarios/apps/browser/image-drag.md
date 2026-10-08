---
id: apps.browser.image-drag
title: Browser の画像を Notes と Text Editor へ drag すると、画像と画像の URL が渡る
status: active
areas: [browser, libbrowser, dnd]
paths: [userland/desktop/browser/shell/shell.c, userland/desktop/libbrowser/page/link.c, userland/desktop/libbrowser/view/view.c, userland/desktop/include/browser/browser.h]
machine: either
human: look
since: ws189-p003
---

## 目的
Browser を画像の drag の元に（WS189 p003、libbrowser の `browser_view_image_at`）: 画像を押して動かすと、画像（PNG）と src の URL（文字）の drag になること。

## 準備
画像のある頁（例: file の HTML に `<img src="photo.png">`）を Browser で開き、Notes と Text Editor を開く。

## 操作と確認
1. 操作: 頁の画像を押して 20 px 動かし、Notes の頁で離す。`aat mark start` は押す前。
   確認事項: 画像。正解: `ZBROWSER DND drag image size=WxH bytes=N url=file:///.../photo.png errno=0`、`KWL DATA drag start ... types=3`、`NOTES DND drop`。確認方法: log、撮影。
2. 操作: 同じく画像を Text Editor へ drag して離す。
   確認事項: URL。正解: Text Editor に画像の URL の文字。確認方法: log、撮影。
3. 操作: 頁の文字の上で押して動かす。
   確認事項: 画像の無い所。正解: drag の行が無く、今の動き（選択など）。確認方法: log。

## 合格
1〜3。
