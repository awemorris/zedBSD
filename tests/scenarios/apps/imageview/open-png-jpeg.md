---
id: apps.imageview.open-png-jpeg
title: Image Viewer で PNG と JPEG を開く
status: active
areas: [imageview, picture]
paths: [userland/desktop/imageview/, userland/desktop/picture/]
machine: either
human: look
since: ws128
---

## 目的
Image Viewer が PNG と JPEG を開いて描くことを確かめる（UAT F5）。

## 準備
`sample.png`・`sample.jpg`（320x200）を `/tmp/aat-samples/` に置く。

## 操作と確認
1. 操作: user で `imageview /tmp/aat-samples/sample.png`。
   確認事項: 開き。正解: `IMAGEVIEW IMAGE path=/tmp/aat-samples/sample.png format=png width=320 height=200 …`。確認方法: log、撮影。
2. 操作: 閉じて、`imageview /tmp/aat-samples/sample.jpg`。
   確認事項: 開き。正解: `IMAGEVIEW IMAGE path=…sample.jpg format=jpeg width=320 height=200 …`。確認方法: log、撮影。

## 合格
2 つの log。見え（虹色の帯に白い四角）は needs-person。
