---
id: desktop.bar.volume-slider
title: 音量の slider を drag しても止まらず、確認の音は 1 回
status: active
areas: [compositor, bar, sound]
paths: [userland/desktop/wayland/volume.c, userland/base/audiod/]
machine: either
human: none
since: BUG-170
---

## 目的
BUG-170（slider の drag で確認の音を鳴らし続けて止まる）が直ったままであることを確かめる（UAT D1）。

## 準備
desktop。今の音量を log の `ZWL VOLUME restored value=` か `ZWL VOLUME set value=` で控える（最後に戻す）。

## 操作と確認
1. 操作: bar の音量の icon（`ZWL VOLUME icon` の位置）を click（状態の島のパネルが開く、WS192）。
   確認事項: パネル。正解: 開く。確認方法: log `KWL STATUS panel open … sound=` と `KWL STATUS item name=volume x= y= width= height=`（slider の track の左・knob の上端・track の幅・knob の高さ）。
2. 操作: slider の 20% の所から 80% の所まで 1 秒ほどかけて drag。
   確認事項: 値と音。正解: `ZWL VOLUME set value=80 … final=1` 前後、確認の音（`ZWL VOLUME feedback`）は 1 回以下。確認方法: log。
3. 操作: すぐに画面を撮る。
   確認事項: 応答。正解: 5 秒以内に撮影が返る（compositor が止まっていない）。確認方法: 撮影の時間。
4. 操作: Esc、音量を元に戻す（同じ slider で）。
   確認事項: 値。正解: 元の値。確認方法: log。

## 合格
1〜3 の正解。

## 注記
slider の x: `item name=volume` の x + 14 px が 0%、幅は width − 28 px（knob の幅、`status-panel.c`）。y は item の y + height/2。音が実際に鳴るかは UAT。QEMU に audiod の音の device が無い時は `ZWL VOLUME reachable=0` で、値の変化だけを見る。
