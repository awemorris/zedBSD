---
id: desktop.osk.open-close
title: 画面 keyboard を右下・左下の角からの斜めの drag で開いて閉じる
status: active
areas: [compositor, osk]
paths: [userland/desktop/wayland/keyboard.c, userland/desktop/wayland/keyboard-layout.c]
machine: either
human: none
since: ws102
---

## 目的
画面 keyboard（flick の panel と QWERTY の panel）が角の gesture（pointer でも可）で開閉することを確かめる。

## 準備
desktop。App Home は閉じている。

## 操作と確認
1. 操作: 画面の右下の角から左上へ斜めに 200 px drag。
   確認事項: flick の panel。正解: 右に縦長の panel。確認方法: log `ZWL OSK open kind=flick x= y= width= height=`、撮影。
2. 操作: 同じ drag をもう一度。
   確認事項: panel。正解: 閉じる。確認方法: log `ZWL OSK close kind=flick`。
3. 操作: 左下の角から右上へ斜めに 200 px drag。
   確認事項: QWERTY の panel。正解: 下に横長の panel。確認方法: log `ZWL OSK open kind=qwerty`、`ZWL OSK qrect … label=…`（鍵の位置）、撮影。
4. 操作: 同じ drag でもう一度。
   確認事項: panel。正解: 閉じる。確認方法: log `ZWL OSK close kind=qwerty`。

## 合格
1〜4 の正解。
