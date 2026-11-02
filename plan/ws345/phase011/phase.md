<!-- awesome-plan project=zedbsd record=ws345-p011 -->

# ws345-p011 受け入れと回帰

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

WS345 の目標（窓の event が止まらない）を測って確かめ、全体の回帰を流す。

## 手順と試験

1. `plan/ws345/tests/stall-probe.c` を `_OWN` の形に直した受け入れの試験 `plan/ws345/tests/threaded-latency.sh`（窓の代わりの thread と描画の代わりの thread）: 次の各場面で、窓の代わりの loop の event の遅れの最長が 20 ms 以下、100 ms を超える event が 0、描画の代わりの thread は resize の後 100 ms 以内に present する。
   - timer の script が 10 s 回る、mousedown の listener が 2 s 回る
   - 同期の XHR が connect の掛かる listener を待つ（30 s、loader の時間切れまで）、read の掛かる listener を待つ
   - `document.write` の `<script src>` が connect の掛かる listener を待つ
   - 6000 個の box の頁の resize、3000 × 3000 の PNG の decode
   - cross-origin の iframe の無限 loop
2. ThreadSanitizer の variant で 1 の全場面と host-threaded-view・host-frames。
3. 回帰: host-build（plain・asan）warning 0、host-* が前と同じ、js、test262 の部分集合（前後で落ちた一覧が同じ）、http・http-async・loader・fetch・component、WPT の reftest の見本と DOM の試験、段 1 の前後、cross-build、BUG-411 の bench。
4. zedBSD の QEMU（boot-test.sh と browser の画面の PNG）、Linux の Keiland の guest（SSH と画面の PNG）、Windows の Wine。本物の Windows・実機はユーザーが拾える試験の一覧に（Q1）。

## 受け入れ

- 1 の上限を全場面で満たし、2・3 に新しい失敗が無く、4 で三つの platform が頁を出す。
