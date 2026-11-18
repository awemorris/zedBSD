---
id: desktop.osk.emoji
title: 画面 keyboard の絵文字の面から絵文字を打つ
status: active
areas: [compositor, osk]
paths: [userland/desktop/wayland/keyboard.c, userland/desktop/wayland/keyboard-layout.c]
machine: either
human: look
since: ws102-p022
---

## 目的
flick の panel の「絵文字」tab から絵文字が欄に入ることを確かめる。

## 準備
user で `textedit /tmp/aat-work/emoji.txt` を開いて本文を click。flick の panel を開く。

## 操作と確認
1. 操作: 「絵文字」の tab を tap（panel の tools の 2 段目の 4 つ目）。
   確認事項: 絵文字の面。正解: `ZWL OSK tool face=emoji category=…`、`ZWL OSK erect …` の行、絵文字の格子。確認方法: log、撮影。
2. 操作: 最初の絵文字（`ZWL OSK erect category=C index=0` の位置）を tap。
   確認事項: 入力。正解: `ZWL OSK emoji commit sent=1 text=E`、本文に E。確認方法: log、撮影。
3. 操作: panel を閉じ、Ctrl+S。
   確認事項: file。正解: E（4 byte の UTF-8 など）を含む。確認方法: file を読む。

## 合格
1〜3 の正解。絵文字の色の見えは needs-person。

## 注記
tab の行: panel の上から 36+6+44+6 px、高さ 30、4 列（`keyboard_tool_rect`）。
