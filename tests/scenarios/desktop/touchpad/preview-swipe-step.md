---
id: desktop.touchpad.preview-swipe-step
title: 3 本指の tap の preview を 2 本指で選ぶ（1 回の swipe で 1 つ）、上端・下端・横の端の 2 本指
status: draft
areas: [touchpad, compositor, switcher]
paths: [userland/desktop/wayland/swipe.c, userland/desktop/wayland/switcher-shell.c, userland/desktop/wayland/touchpad.c]
machine: hardware
human: hands
since: ws142-p009
---

## 目的
preview（switcher）の 2 本指の swipe が 1 回で 1 つ（BUG-215）、下端からの 2 本指の上で App Home・上端からの 2 本指の下で WiseView（2026-10-13 ユーザーの指示、touch panel の端と同じ。前は上端で App Home、ws181-p008）、App Home と WiseView の間の行き来、App Home の上で 2 本指で下へ払って閉じる（ws181-p012、2026-10-13 ユーザー「トラックパッドも下へ払って閉じる形に変えます。」、touch panel の Home の上の下への drag と同じ）、全画面から下端の 2 本指の上で最大化（BUG-228）、端の帯に 2 本のうち 1 本があれば端の gesture（ws181-p008）になることを人の指で確かめる。QEMU の注入の版は plan/ws142/tests/p010-guest.sh の 2.・6.・7.・8. と plan/ws142/tests/p003-guest.sh の 1.・2.・7b.・7c.・7d.・7e. と plan/tools/desktop-guest/p009-guest.sh の 3.。

## 準備
5330 の desktop で 3 つ以上の app を開く。

## 操作と確認
1. 操作: 3 本指で tap、2 本指で長く右へ。
   確認事項: 選び。正解: 1 つだけ右へ（`ZWL SWITCH step … via=pad` が 1 回）。確認方法: 人、log。
2. 操作: 2 本指で下へ（または 1 本指で tap）。
   確認事項: 確定。正解: 選んだ app へ（`ZWL SWITCH commit … via=pad-swipe`・`via=pad`）。確認方法: 人、log。
3. 操作: 下端から 2 本指で上へ（1 本は端に、もう 1 本は少し内側に置く）。
   確認事項: App Home。正解: desktop が縮んで奥へ消え、App Home が奥から大きくなって現れる（指に付く。`KWL HOME pad swipe closing=0`・`KWL HOME open via=pad`）。少しだけ上げて離すと戻る（`KWL HOME pad back`）。速く払うと短くても開く。touch panel の下端からの swipe up と同じ見え方。確認方法: 人、log。
3b. 操作: App Home の上で、上端から 2 本指で下へ。続けて WiseView の上で、下端から 2 本指で上へ。
   確認事項: 行き来。正解: App Home がすぐ消えて WiseView が指に付いて開く（`KWL HOME close via=pad-top`・`KWL WISEVIEW opening`）。WiseView がすぐ消えて App Home が指に付いて開く（`KWL WISEVIEW close via=pad-bottom`・`KWL HOME open via=pad`）。確認方法: 人、log。
3c. 操作: App Home の上で、pad の真ん中から 2 本指で下へ払う。次に App Home を開き直し、少しだけ下げて上へ戻してから離す。最後に App Home の上で、2 本指で横へ払う・下端から 2 本指で上へ払う。
   確認事項: 下へ払って閉じる。正解: App Home が指に付いて閉じ、desktop が戻る（`KWL GESTURE kind=down2`・`KWL HOME pad swipe closing=1`・`KWL HOME close via=pad`）。戻して離すと開いたまま（`KWL HOME pad stays`）。速く払うと短くても閉じる。touch panel の Home の上の下への drag と同じ見え方。横へは page（閉じない）、下端から上へは何もしない（touch panel の Home の上の下端の swipe up と同じ）。確認方法: 人、log。
4. 操作: Terminal を F11 で全画面にし、下端から 2 本指で上へ。
   確認事項: 最大化。正解: 全画面から最大化（`ZWL GLASS fullscreen-leave … via=bottom2`、WiseView は開かない）。確認方法: 人、log。
5. 操作: 左の端に 1 本、内側に 1 本を置いて 2 本指で右へ（右の端から左へも）。
   確認事項: 仮想 desktop。正解: 隣の desktop へ（`KWL GLASS desktop=N via=pad`）。確認方法: 人、log。

## 合格
人の判断（感触）と log の行。
