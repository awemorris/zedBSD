---
id: apps.notes.draw-stroke
title: Notes で線を描く
status: active
areas: [notes]
paths: [userland/desktop/notes/]
machine: either
human: look
since: ws128
---

## 目的
Notes が pointer の drag で線を描くことを確かめる。

## 準備
App Home から Notes を開く（`NOTES START`）。

## 操作と確認
1. 操作: 頁の中で左上から右下へ drag。
   確認事項: 線。正解: `NOTES STROKE page=… id=… points=N strokes=M`（N > 1）。確認方法: log、撮影（人が見る）。

## 合格
`NOTES STROKE` の行。
