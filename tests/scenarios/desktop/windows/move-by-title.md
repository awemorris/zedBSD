---
id: desktop.windows.move-by-title
title: title bar を drag すると窓が動く
status: active
areas: [compositor, windows]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/toplevel.c]
machine: either
human: none
since: ws035
---

## 目的
浮いた窓を title bar の drag で動かせることを確かめる（UAT 2.3 の pointer の側。指は desktop.touchpad.gestures）。

## 準備
App Home から Files を開く（浮いた窓、最大化でない）。

## 操作と確認
1. 操作: title bar の空いた所（題の左、操作の部品の無い所）から右下へ 120・80 px drag。
   確認事項: 窓の位置。正解: 窓が同じ量（±8 px）だけ動き、離すと止まる。確認方法: log `ZWL GLASS moved surface=S x= y=`（前の位置は `ZWL GLASS launch … to=x,y`）、撮影。

## 合格
動いた量が drag と合う。
