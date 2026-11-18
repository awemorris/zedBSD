---
id: desktop.windows.close-minimize
title: 最小化と apps bar からの復帰、閉じる button
status: active
areas: [compositor, windows, apps-bar]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/apps-bar.c]
machine: either
human: none
since: ws035
---

## 目的
title bar の最小化と閉じる button、apps bar の icon での復帰を確かめる（UAT B6）。

## 準備
App Home から Files を開く（浮いた窓）。

## 操作と確認
1. 操作: title bar の右から 3 つ目の button（最小化）を click。
   確認事項: 窓。正解: 隠れる。確認方法: log `ZWL GLASS minimize surface=S`、撮影。
2. 操作: apps bar の Files の icon を click。
   確認事項: 窓。正解: 戻る。確認方法: log `ZWL APPS icon app=… x y width height` で icon の位置、click の後の撮影（窓が見える）。
3. 操作: title bar の右端の button（閉じる）を click。
   確認事項: 窓と app。正解: 窓が消え、Files が終わる。確認方法: log `ZWL UNMAP client=C surface=S`。

## 合格
1〜3 の正解。

## 注記
浮いた窓の button は title bar の右端から 26 px、34 px おき（閉じる・最大化・最小化、`shell.c` の `button_centre`）。
