---
id: desktop.home.super-key
title: Windows キーを押して離すと App Home が開き、もう一度で閉じる
status: active
areas: [compositor, home, keyboard]
paths: [userland/desktop/wayland/home.c, userland/desktop/wayland/seat.c, userland/desktop/wayland/super-tap.c]
machine: either
human: none
since: ws142-p002
---

## 目的
Windows（Super）キーの単独の押下で App Home が開閉することを確かめる（UAT 4.1・4.2）。

## 準備
desktop（os.boot.session-up）。App Home は閉じている。

## 操作と確認
1. 操作: Windows キーを押して離す。
   確認事項: App Home。正解: 開く（icon の格子と検索の欄）。確認方法: log `ZWL SUPER home` と `ZWL HOME open via=super`、撮影。
2. 操作: もう一度 Windows キーを押して離す。
   確認事項: App Home。正解: 閉じる。確認方法: log `ZWL HOME close`（または `ZWL HOME closed`）。

## 合格
開いて閉じる。
