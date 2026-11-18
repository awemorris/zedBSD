---
id: desktop.wiseview.super-tab
title: Windows+Tab で Wiseview が開き、App Home は開かない
status: active
areas: [compositor, wiseview, keyboard]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/super-tap.c]
machine: either
human: none
since: ws142-p002
---

## 目的
Super と Tab の組で Wiseview（窓の一覧）が開き、Super の単独の押下とは区別されることを確かめる（UAT 4.3）。

## 準備
desktop に窓が 1 つ以上（例: App Home から Files を開く）。

## 操作と確認
1. 操作: Windows+Tab。
   確認事項: Wiseview。正解: 開く（窓の縮小の一覧）。App Home は開かない。確認方法: log `ZWL WISEVIEW opening`、`ZWL HOME open` が無い、撮影。
2. 操作: Esc。
   確認事項: Wiseview。正解: 閉じる。確認方法: log `ZWL WISEVIEW close`。

## 合格
1・2 の正解。
