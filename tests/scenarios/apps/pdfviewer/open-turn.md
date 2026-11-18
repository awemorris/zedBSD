---
id: apps.pdfviewer.open-turn
title: PDF Viewer で 2 頁の PDF を開き、頁を送る
status: active
areas: [pdfviewer, libpdf]
paths: [userland/desktop/pdfviewer/, userland/desktop/libpdf/]
machine: either
human: look
since: ws127
---

## 目的
PDF Viewer が PDF を開き、頁を送れることを確かめる（UAT F3）。

## 準備
`sample.pdf`（2 頁、「AAT page 1」「AAT page 2」）を `/tmp/aat-samples/` に置く。

## 操作と確認
1. 操作: user で `pdfviewer /tmp/aat-samples/sample.pdf`。
   確認事項: 開き。正解: `PDFVIEWER READY … pages=2`（か `PDFVIEWER OPEN path=… pages=2`）。確認方法: log、撮影。
2. 操作: 窓の中を click、Page Down。
   確認事項: 頁。正解: `PDFVIEWER PAGE shown=…` が 2 頁目に変わる。確認方法: log、撮影（人が見る）。

## 合格
1・2 の log。
