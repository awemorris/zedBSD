<!-- awesome-plan project=zedbsd record=ws345-p008 -->

# ws345-p008 style の thread

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

style・CSS・layout・display list の組み立てを `browser-style` の thread で行う（D4、design.md §4.2）。

## file

- `view/`・`page/`（`page/geometry.c`・`page/sheets.c`・`page/fonts.c`・`page/script.c` の `page_needs_layout`）・`bind/computed.c`・`bind/geometry.c`: request（描画の更新・style の読み・形の読み・当たり判定）と返事、DOM の世代、古くない時の直の読み。
- `css/`・`layout/`・`text/`: style の engine と layout の font の face を style の thread の物に。
- layout・style の中から JS・資源・heap に触る所（R2 の一覧: `page_load_images`・`page_load_media`・`bind_document_fonts_settle`・`bind_animation_sync`・`bind_window_set_viewport`・`page_frames_size`・ResizeObserver・container query・画像の到着）を、貸しの前か後の script の thread の仕事にし、返事に「後でする事」の一覧を持たせる。最初に全部洗い直す。
- style の thread は loader を使わない。sheet・font・背景の画像の取得と完了の callback は frame の script の thread で、style の thread は貸しの間に bytes と解いた結果を読む（design.md §4.3）。

## 手順

1. request の queue と返事（condition）。script の thread は貸しの間 agent の lock を持ったまま待つ。貸しの間は heap の collect を止める深さを上げ、collect は lock の持ち主だけ（assert、design.md §4.2）。
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

## 引き継ぎ（2026-11-02 18:29、P5 の p004・p005 から。main で確かめてから使う）

1. VM の agent は view ごと。task の番号・止める要求・期限は `struct vm_agent`（vm.h）にあり、view/front.c の front が持つ。頁の heap は `view_make_page` で加わる（`vm_heap_set_agent`）。貸しの間の「回収を止める深さ」は `struct vm_heap`（vm/internal.h、vm/heap.c の `vm_heap_alloc`・`vm_heap_collect`）に置く。
2. script の thread の loop は view/front.c の `front_loop`。箱の 1 件ずつを task として走らせ、次に `browser_view_process` を task として、次に `front_publish` → `view_fill_snapshot`。fill は `browser_view_document_height` と `scroll_range`（`view_update` で layout）、`front_paint`（`browser_view_draw_pixels` で描く）を呼ぶ。これらが style の thread への要求になる主な呼び手。
3. p005 が足した `_Thread_local` の一時の状態は、計算を走らせる thread で設定されるので、style・layout が `browser-style` に移ると script の thread が設定した値は見えない。要求に載せて運ぶ: bind/computed.c の `computed_pseudo`・`computed_pseudo_name`・`computed_zoom`、css/cascade.c の `cascade_animation_suppressed`、layout/position.c の `position_remember`、page/svg.c の `svg_view_*`。
4. script の thread は front.c の `front_current`（thread ごとの印）で自分を知る。style の thread にも同じ物が要る。thread をまたぐ呼び出しは base/thread.h の `wb_mutex`・`wb_cond`。新しい thread は `wb_thread_start(name, stack, run, arg, &thread)`（名前は 15 byte まで、"browser-style"）。
5. 試験: plan/ws345/tests/host-threaded-view.{c,sh} は engine の object を link するので、貸しの数などの内部の口を試験から読める。plan/ws345/tests/raster-cost.c は絵の費用の基準（6000 box・800×600 で描くのに 16.6 ms、大きさの変更の後の layout と描画で 318 ms）。
6. bug411 の bench の基準（c7d3d9f1bb、3 回の中央）: big-object-reads 1037、compile-many-names 13、computed-after-change 35、grow-with-lookups 69 ms。p005 の後 1038・13・36・71 ms。
7. P8 の p006 と view/view.c（描画の道）・view/front.c（`front_paint`・`render_pixels`）・paint/ が重なる。p008 は view/・page/・css/・layout/・bind/ を触る。p006 の merge の後に始める（Q1）。
8. この Queue には zedBSD の amd64・arm64（config-rpi5）・riscv64（config-visionfive2-keiland）の libbrowser.so の build（warning 0）も入る。
