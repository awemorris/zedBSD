---
id: desktop.touchpad.wiseview-swipe
title: WiseView を 2 本指で選ぶ（左右 1 回で 1 つ、下で確定）
status: draft
areas: [touchpad, compositor, wiseview]
paths: [userland/desktop/wayland/swipe.c, userland/desktop/wayland/touchpad.c, userland/desktop/wayland/shell.c]
machine: hardware
human: hands
since: ws142-p009
---

## 目的
WiseView の中の 2 本指の swipe が 1 回で 1 つ選びを動かし、下の swipe で確定することを人の指で確かめる（BUG-216、ws142-p007 §2）。QEMU の注入の版は plan/ws142/tests/p010-guest.sh の 6.。

## 準備
5330 の desktop で 3 つ以上の窓を開く。

## 操作と確認
1. 操作: 上端から 2 本指で下（2026-10-13 ユーザーの指示、touch panel の上端と同じ。前は下端から上）。
   確認事項: WiseView。正解: 指に付いて開き（`KWL GESTURE kind=top2`・`KWL WISEVIEW gesture via=pad`）、上に「Wiseview」の題の文字が無い、下に「Swipe left or right to choose · Swipe down to open」。確認方法: 人、撮影。
2. 操作: 2 本指で長く右へ（指を離さずに 3 cm ほど）。
   確認事項: 選び。正解: 1 つだけ右へ（`ZWL WISEVIEW select step=+1 via=swipe` が 1 回）。確認方法: 人、log。
3. 操作: 2 本指で下へ。
   確認事項: 確定。正解: 選んだ窓で WiseView が閉じる（`ZWL WISEVIEW select surface=… via=swipe`）。確認方法: 人、log。

## 合格
人の判断（感触、8 mm の閾値）と log の行。
