<!-- awesome-plan project=zedbsd record=ws345-p014 -->

# ws345-p014 応答しない頁の問い

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §9。
- 状態: planned
- 出所: 2026-11-02 ユーザーの決定（J2 の答え、ws.md）。

## 目的

script が決まった時間走り続けて頁が応答しない時に、Chrome・Firefox のように「このページは応答していません」の問い（待つ／止める）を出し、「止める」で p004 の打ち切りにより script を終わらせる。窓の thread は libbrowser の仕事をしない（決定 1）ので、問いの timer と UI は shell の窓の側にあり、libbrowser の front の snapshot を読むだけ。

## 決め事

- 閾値: 同じ task が 10 s 走り続けたら出す（`SHELL_UNRESPONSIVE_MS` 10000）。「待つ」の後は同じ task がさらに 10 s 走ったら再び出す。task が終われば問いは消える（利用者の操作は要らない）。
- 文言（英語の原文、libkeiland の翻訳の表 `kl_tr` に足す）: 題「This page isn't responding」、本文「A script on <頁の題、無ければ host> has been running for <N> seconds. You can wait for it or stop it.」、button「Wait」（既定）と「Stop script」。日本語: 「このページは応答していません」「<頁> の script が <N> 秒間動き続けています。待つか、止められます。」「待つ」「script を止める」。
- 「Stop script」は今の task だけを打ち切る（頁は残り、次の event・timer は普通に走る）。同じ頁が何度も止まらなくなる場合も毎回聞く。

## file

- `include/browser/browser.h`・`view/front.c`: snapshot に今の task の番号と始めた時刻（単調な時計、ms）。`browser_view_script_busy(view, &task, &milliseconds)`（窓の thread、snapshot を読むだけ）と `browser_view_stop_script(view, task)`（その task の番号を打ち切りに書く。task が変わっていれば何もしない）。
- shell（`userland/desktop/browser/shell/`、zedBSD・Linux の Keiland と Windows で共通）: 窓の loop の timer（`kl_app_dispatch` の待ちの期限に足す）、問いの表示。表示は libkeiland の窓の中の dialog（Keiland では libkeiland の ui の dialog、Windows でも同じ libkeiland の描画）。titlebar の操作と resize は問いの間も効く。
- libkeiland の翻訳の表（文言）。

## 試験

- host の試験（`plan/ws345/tests/host-threaded-view.c` に足す）: 12 s の loop の task で `script_busy` が 10 s を超えた値を返す、`stop_script` で 300 ms 以内に task が終わり頁は残る（次の timer が走る）、古い task の番号の `stop_script` は今の task を止めない、loop の無い頁で busy が 0。
- shell: zedBSD の QEMU と Windows の Wine で 30 s の loop の頁を開き、10 s 後の画面の PNG に問いが出ている、「Stop script」を入力の道具で押すと問いが消えて頁が応答する（画面の PNG）。Linux の Keiland の guest は p011 で。
- 回帰: host-build warning 0、host の試験、段 1、cross-build。

## 受け入れ

- 上の試験が通り、問いの間も窓の event（titlebar・resize・再描画）が止まらない。
