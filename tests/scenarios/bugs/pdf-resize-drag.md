---
id: bugs.pdf-resize-drag
title: PDF Viewer の窓の resize の drag の間は頁を描き直さず、止まった後に 1 回描く
status: active
areas: [pdfviewer]
paths: [userland/desktop/pdfviewer/view.c]
machine: either
human: none
since: BUG-259
---

## 目的
BUG-259（resize の drag の応答が悪い。event ごとの重い描き直し）が戻っていないかを、page 1 の raster の回数で確かめる。

## 準備
AAT の image と試験の file（`/tmp/aat-samples/sample.pdf`）。

## 操作と確認
1. 操作: `pdfviewer --width=900 --height=600 sample.pdf` を開き、右下の角を約 1 秒で 300x180 内へ drag、止めて離す。
   確認事項: drag の間は描き直さない。正解: `KWL RESIZE start`、`PDFVIEWER RESIZE settled`、`PDFVIEWER RASTER index=0` が全部で 4 回以下。確認方法: log、撮影 held・after。

## 合格
行が出て、raster が 4 回以下。超えれば BUG-259 の再現（fail）。

## 注記
体感（5330）は UAT。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
