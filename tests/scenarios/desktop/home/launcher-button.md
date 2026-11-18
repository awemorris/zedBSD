---
id: desktop.home.launcher-button
title: system bar の左端の launcher で App Home が開く
status: active
areas: [compositor, home, bar]
paths: [userland/desktop/wayland/home.c, userland/desktop/wayland/shell.c]
machine: either
human: none
since: ws035
---

## 目的
pointer で App Home を開けることを確かめる。

## 準備
desktop。App Home は閉じている。

## 操作と確認
1. 操作: system bar の左端の launcher（格子の icon）を click。
   確認事項: App Home。正解: 開く。確認方法: log `ZWL HOME open via=launcher`、撮影。
2. 操作: Esc。
   確認事項: App Home。正解: 閉じる。確認方法: log `ZWL HOME close`。

## 合格
開いて閉じる。

## 注記
launcher の位置は log に無い。compositor の作りでは bar の左端から 10 px、26 px 角、bar の高さの中（`shell.c` の `BAR_LAUNCHER_*`）。
