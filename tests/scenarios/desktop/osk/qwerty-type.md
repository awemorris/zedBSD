---
id: desktop.osk.qwerty-type
title: QWERTY の画面 keyboard で Text Editor に打つ
status: active
areas: [compositor, osk, textedit]
paths: [userland/desktop/wayland/keyboard.c, userland/desktop/wayland/keyboard-layout.c, userland/desktop/wayland/text-input.c]
machine: either
human: none
since: ws102-p006
---

## 目的
QWERTY の panel の鍵が焦点の欄に文字を送ることを確かめる。

## 準備
入力方式は None か direct。user で `textedit /tmp/aat-work/qwerty.txt` を開き、本文を click。

## 操作と確認
1. 操作: 左下の角から QWERTY の panel を開く。
   確認事項: panel。正解: `ZWL OSK open kind=qwerty`。確認方法: log。
2. 操作: `a`・`b`・`c` の鍵を順に tap（位置は log の `ZWL OSK qrect … label=a` など）。
   確認事項: 送られた文字。正解: `ZWL OSK send …` が 3 回、Text Editor に `abc`。確認方法: log、撮影。
3. 操作: panel を閉じ、Ctrl+S。
   確認事項: file。正解: `abc`。確認方法: file を読む。

## 合格
file が `abc`。
