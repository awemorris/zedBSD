<!-- awesome-plan project=zedbsd record=ws345-p008 -->

# ws345-p008 style の thread

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

style・CSS・layout・display list の組み立てを `browser-style` の thread で行う（D4、design.md §4.2）。

## file

- `view/`・`page/`（`page/geometry.c`・`page/sheets.c`・`page/fonts.c`・`page/script.c` の `page_needs_layout`）・`bind/computed.c`・`bind/geometry.c`: request（描画の更新・style の読み・形の読み・当たり判定）と返事、DOM の世代、古くない時の直の読み。
- `css/`・`layout/`・`text/`: style の engine と layout の font の face を style の thread の物に。
- layout・style の中から JS を呼ぶ所（ResizeObserver・container query の結果・画像の到着など）を「後で出す event」の印にする（R2）。
- style の thread の loader の client（sheet・font・背景の画像）。

## 手順

1. request の queue と返事（condition）。script の thread は貸しの間 agent の lock を持ったまま待つ。
2. 古くない時の直の読み（DOM の世代と、style の thread がその frame の物を書くのは貸しの間だけ）。
3. 後で出す event。
4. 計測: `offsetWidth` を 3 万回の頁で貸しの回数と時間（BUG-411 の bench に足す）。

## 試験

- `plan/ws345/tests/host-threaded-view.c` に足す: getComputedStyle・offsetWidth・getBoundingClientRect の値が `_CALLER` と同じ（computed・geometry の既存の DOM 試験を `_OWN` でも流す）。DOM を変えない読みの連続で貸しは 1 回。
- WPT の reftest の見本（`run-wpt-reftests.py` 既定の 100 件）と DOM の試験（`run-dom-tests.py`）が前と同じ。
- ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。BUG-411 の bench が遅くならない（2% 以内）。
