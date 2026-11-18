---
id: desktop.lock.lock-unlock
title: Super+L で lock し、password で戻る
status: active
areas: [compositor, lock]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/seat.c, userland/base/login/]
machine: either
human: none
since: ws035-p102
---

## 目的
画面の lock と password での解除を確かめる。

## 準備
desktop。

## 操作と確認
1. 操作: Super+L。
   確認事項: lock の画面。正解: `ZWL LOCK locked reason=… user=user`、lock の画面。確認方法: log、撮影。
2. 操作: `user`、Enter。
   確認事項: 解除。正解: `ZWL LOCK unlocked`、元の desktop。確認方法: log、撮影。

## 合格
1・2 の正解。
