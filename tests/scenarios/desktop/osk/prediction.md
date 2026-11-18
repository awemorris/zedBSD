---
id: desktop.osk.prediction
title: 画面 keyboard の予測: か・ん で候補が出て、tap で置き換わる
status: active
areas: [compositor, osk, ime]
paths: [userland/desktop/wayland/keyboard.c, userland/desktop/ime/]
machine: either
human: look
since: ws166-p003
---

## 目的
flick の panel の「候補」tab に、打った仮名の読みからの予測が出て、tap で読みが語に置き換わることを確かめる（WS166）。

## 準備
入力方式 Japanese。user で `textedit /tmp/aat-work/predict.txt` を開いて本文を click。flick の panel を開く（desktop.osk.open-close の 1）。panel が kana の面でなければ、右下の面の鍵（`A`・`1`・`あ`）を kana まで押す（`ZWL OSK face name=kana`）。

## 操作と確認
1. 操作: 「か」の鍵を tap、「わ」の鍵を押したまま上へ flick（ん）。
   確認事項: 読みと予測。正解: `ZWL OSK reading=かん`、`ZWL OSK predictions … reading=かん count=N`（N > 0）、候補の tab に語が並ぶ。確認方法: log、撮影。
2. 操作: 最初の候補（`ZWL OSK crect slot=0` の位置）を tap。
   確認事項: 置き換え。正解: `ZWL OSK candidate commit sent=1 slot=0 word=W reading=かん`、本文に W。確認方法: log、撮影。
3. 操作: panel を閉じ、Ctrl+S。
   確認事項: file。正解: W を含む。確認方法: file を読む。

## 合格
1〜3 の正解。

## 注記
flick の鍵は panel の下に 4×4、鍵の辺は画面の高さ/11（64〜96 px）、間 6 px（`keyboard.c` の `keyboard_key_rect`）。か は 1 行目の 2 列目、わ は 4 行目の 2 列目、上の flick は 30 px。
