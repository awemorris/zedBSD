---
id: bugs.osk-flick-terminal
title: 画面 keyboard の flick のかなを Terminal で: 候補が二重に入らない、送り仮名つきの変換、かなの面で IME が ON
status: active
areas: [osk, ime, terminal]
paths: [userland/desktop/wayland/keyboard.c, userland/desktop/ime/ja-engine.c, userland/desktop/terminal/window.c]
machine: either
human: none
since: ws102-p026
---

## 目的
2026-10-13 の UAT の指摘（「あい」の後に「愛」を選ぶと「あい愛」になる、送り仮名つきの変換ができない、かなの面で IME が ON にならない）が今も起きないかを Terminal で確かめる。

## 準備
AAT の image。入力方式を日本語（method 1）にし（helper、終わりに戻す）、App Home から Terminal を開き、`echo ` を打つ。

## 操作と確認
1. 操作: 右下の角の swipe で flick の panel を開き、kana の面にする。
   確認事項: IME が ON。正解: `KWL OSK ime face=kana on=1`。確認方法: log。
2. 操作: 「あ」を tap、「あ」を左へ flick（い）、候補の tab の 1 番目（`KWL OSK crect slot=0`）を tap。
   確認事項: 読みは preedit、選んだ語だけが確定する。正解: `KWL OSK preedit text=あい`、`KWL OSK candidate commit sent=1 slot=0 word=W`、Terminal の `ZTERM IME commit` が 1 回だけで W、`ZTERM IME delete` が無い。確認方法: log、撮影 chosen。
3. 操作: 「や」を下へ（よ）、「ま」を上へ（む）、空白、改行。
   確認事項: 送り仮名つきの変換。正解: `KWL OSK convert slot=0 word=読む reading=よむ`、`ZTERM IME commit bytes=6 text=読む`。確認方法: log、撮影 converted。
4. 操作: panel を閉じ、Ctrl+C、Alt+Space で direct に戻す。

## 合格
1〜3 の全部。2 の commit が 2 回以上か delete があれば二重入力の再現（fail）。

## 注記
QEMU の osk-guest の `p026` の段（`plan/ws102/tests/osk-guest.sh`）が同じことを英字・数字の面・次のApp・QWERTY の配置まで含めて確かめる。helper は `plan/tools/aat/scenarios/helpers_bugs.py`。
