---
id: desktop.lock.wheel-card
title: lock の画面で wheel を上へ 2 notch 回すと card、password で戻る
status: active
areas: [compositor, lock]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/lock-swipe.c, userland/desktop/wayland/seat.c]
machine: either
human: none
since: ws187-p002
---

## 目的
touchpad も touchscreen も無い機械のために、mouse の wheel の上で lock の画面を開けることを確かめる（ws187-p002）。

## 準備
desktop。

## 操作と確認
1. 操作: Super+L。
   確認事項: lock。正解: `KWL LOCK locked reason=key`。確認方法: log。
2. 操作: 画面の中央で wheel を上へ 2 notch 回す。
   確認事項: スワイプの扱い。正解: `KWL LOCK swipe via=wheel grace=0 manual=1`。確認方法: log、撮影。
3. 操作: 方式の選択が並ぶ（PIN か key が登録済み）なら Password を押してから、password を打って Enter。
   確認事項: 解除。正解: `KWL LOCK unlocked`。確認方法: log。

## 合格
1〜3 の正解。
