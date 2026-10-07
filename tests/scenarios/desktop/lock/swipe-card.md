---
id: desktop.lock.swipe-card
title: 手動の lock は時計だけ、下部から上へのスワイプで方式の選択つきの card、password で戻る
status: active
areas: [compositor, lock]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/lock-swipe.c, userland/desktop/wayland/lock-clock.c]
machine: either
human: look
since: ws187-p002
---

## 目的
lock の画面が最初は大きな時計と案内だけを出し、下部から上へのスワイプで card を出し（手動の lock なので猶予なし）、password で解除できることを確かめる（ws187-p001〜p003）。

## 準備
desktop。

## 操作と確認
1. 操作: Super+L。
   確認事項: 手動の lock。正解: `KWL LOCK locked reason=key user=… manual=1`。時計は大きく中央より上にあり、下に「Swipe up to unlock」があり、card は無い。確認方法: log、撮影（人が見る）。
2. 操作: 画面の下 15% の所から高さの 40% の所まで、左のボタンでドラッグする。
   確認事項: スワイプ。正解: `KWL LOCK swipe via=pointer grace=0 manual=1`。card が出て、時計と重ならない。PIN か key が登録済みなら、方式の選択が並ぶ。確認方法: log、撮影（人が見る）。
3. 操作: password を打って Enter。
   確認事項: 解除。正解: `KWL LOCK unlocked`。確認方法: log。

## 合格
1〜3 の正解。撮影の時計・案内・card の配置は人が見る。
