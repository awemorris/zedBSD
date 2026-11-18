---
id: desktop.windows.open-maximized
title: 最大化の中で開いた app は最大化で開く
status: active
areas: [compositor, windows]
paths: [userland/desktop/wayland/shell.c]
machine: either
human: none
since: ws099-p033
---

## 目的
最大化した窓が前にある時に開いた app が最大化で開くことを確かめる（uat.md 2026-10-05 午後 #4）。

## 準備
Files を開き、title bar の double click で最大化。

## 操作と確認
1. 操作: App Home から Text Editor を開く。
   確認事項: 新しい窓。正解: 最大化で開く。確認方法: log `ZWL GLASS open-docked client=C surface=S …`、撮影。

## 合格
新しい窓の `GLASS open-docked`。
