---
id: apps.pdfviewer.drag-out
title: PDF Viewer の選択の文字と、長押しした画像を他の窓へ drag できる
status: draft
areas: [pdfviewer, dnd]
paths: [userland/desktop/pdfviewer/find.c, userland/desktop/pdfviewer/view.c, userland/desktop/pdfviewer/main.c]
machine: either
human: look
since: ws189-p003
---

## 目的
PDF Viewer を drag の元に（WS189 p003）: 選択の中を押して動かすと文字の drag、頁の画像の上で 0.4 秒止めてから動かすと画像の drag になり、短い press の動きは今と同じ pan であること。

## 準備
画像と文字のある PDF（例: Notes の apps.notes.pdf-insert-image で作った PDF）を PDF Viewer で開き、Text Editor と Notes を重ならないように開く。

## 操作と確認
1. 操作: 文字の一語を drag で選び、その選択の中を押して Text Editor へ動かして離す。`aat mark start` は押す前。
   確認事項: 文字。正解: `PDFVIEWER DND drag text bytes=N errno=0`、`KWL DATA drag drop`、Text Editor にその語。確認方法: log、撮影。
2. 操作: 頁の画像の上で押して 0.6 秒止め、Notes の頁へ動かして離す。
   確認事項: 画像。正解: `PDFVIEWER DND image page=0 object=...`、`PDFVIEWER DND drag image page=0 size=WxH bytes=N errno=0`、`NOTES DND drop`、Notes に画像。確認方法: log、撮影。
3. 操作: 頁の画像の上で押してすぐ（止めずに）動かす。
   確認事項: pan。正解: 画像の drag の行が無く、頁が動く（今と同じ）。確認方法: log、撮影。

## 合格
1〜3。
