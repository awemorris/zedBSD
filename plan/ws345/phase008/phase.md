<!-- awesome-plan project=zedbsd record=ws345-p008 -->

# ws345-p008 style の thread

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared 候補（2026-11-02 P5 q1985-i01。WPT の reftest の見本は未実施、下の「未実施・残り」。判定は Q1）

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

## 結果（2026-11-02 P5、q1985-i01、基点 57bc8e37f5）

commit: 21c33851c3（実装と試験）、81515c6019（試験の直し・design.md）、この記録の commit。

### 作った物

- `view/style.c`・`view/style.h`（新）: `browser-style` の thread（stack は script の thread と同じ 64-bit 8 MiB・32-bit 2 MiB）。request は貸す thread の stack の上の記録で、FIFO（lock と二つの condition: asked・answered）に積み、style の thread は順に一つずつ走らせる。request は script の thread の `css_animation_is_off()` と `layout_try_remembers()`（新、`layout/position.c`）を運び、style の thread はそれを自分の thread の値にしてから仕事を走らせる。貸しの数と待った時間を数える（`view_style_counts`、試験用に `view_front_lend_counts`）。
- `view/front.c`: script の thread が `front_run` の始めに style の thread を作り（`front_enter` でその thread も自分を front の物と知る、引き継ぎ 4）、agent の lender にする（`vm_agent_set_lender`）。view を壊した後に lender を外して止め、join する。P8 が直す `front_note`・`front_publish` の入れ替え・`view_front_process` には触れていない（Q1 の指示）。
- `vm/`: `struct vm_agent` に `lend`・`lend_context`、`vm_heap_lend(heap, work, argument)`（lender の無い heap と borrower の thread の中ではその場で走る）、`vm_heap_lent`。heap の `lent` の深さが上がっている間は、閾値を越えても上限でも collect しない。`vm_thread_set_borrower`（thread ごとの印）の thread は collect せず、script の入口（`interpreter_enter`・`vm_interpret_resume`）と collect の試みを `vm_borrower_slips` が数える（本番の挙動は変えず、試験が 0 を確かめる）。
- 貸す仕事（葉だけ、design.md §4.2 の p008 の実装）: `page_layout` の箱の木（`page_layout_work`: web font の face の追加、`layout_build` と container の再度の layout、iframe の window の大きさ）、`page_paint` の display list と focus の輪・caret（`page_paint_work`）、sticky の移動（sticky の箱が無い時・同じ scroll の時は貸さない）、`geometry_cascade`（getComputedStyle の cascade）、`::before`・`::after`・view transition の擬似要素の cascade（`page/transition.c`）、iframe と一時の layout（`page_lend_layout`: `page/geometry.c` の 2 か所、`page/frame-load.c` の `frame_tree`）と iframe の paint（`page_lend_paint`）、iframe の window の cascade（`bind/style-context.c`）。
- 古くない時は貸さない: `page_needs_layout` が偽なら `geometry_layout` は `page_layout` を呼ばず、`offsetWidth` などは木と box の索引を直に読む（style の thread がその頁の物を書くのは貸しの間だけ）。
- 後でする事（R2 を洗い直した結果）: `page_layout` の前に script の thread で、sheet の集めと link の event（`page_update_styles`）・viewport（`bind_window_set_viewport`）・画像の取得（`page_load_images`）・media の開始（`page_load_media`）。後に、`bind_document_fonts_settle`（promise）・古い layout の解放・世代の記録・`page_animations_sync`（`bind_animation_sync`）。style の thread が `page_image_by_url` で出会った http・https の新しい画像は取得せず `page->images_deferred` に積み、`page_lend` が戻った後に script の thread が取得する（file・data は今のとおりその場で読み解く。loader を使わない）。ResizeObserver・IntersectionObserver の項目は bind の側（script の thread）で box を読んで作るので変えていない。container query は cascade の中（純粋な計算）。画像・sheet・font の到着の callback は loader の dispatch（script の thread）のまま。layout・css・paint・text の code から JS・event・promise の口を呼ぶ所は無い（grep で 0、試験で slips 0）。
- 入れなかった物: sheet の解析と engine の組み立て（`page_update_styles`）は取得と link の event が混ざるので script の thread に残した（Future Work の候補）。style の thread の中から `page_layout` 全体が呼ばれる道（iframe の paint → `bind_frame_viewport` → 親の `node_box` → 親が古い時）は、paint は layout の直後なので起きない前提で、守りは置いていない。
- 変えた試験の file: `plan/ws345/tests/host-threaded-view.c`（(l)〜(p) と、ThreadSanitizer が lavapipe の unload で process を止めないよう `dlopen("libvulkan_lvp.so", RTLD_NODELETE)`）。

### 試験（host、2026-11-02）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan` | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | 39 checks、0 failed。新しい物: (l) dom/computed・geometry・fit-content-computed の console が `_OWN` と `_CALLER` で同じ（28・15・17 行）。(m) 1 回の変更の後の `offsetWidth` 3 万回で貸し 3 回、変更と読みを 1000 回で貸し 1002 回（1 回 51〜60 µs、仕事を含む）。(n) 貸した heap は 16 MiB を確保しても貸しの間 collect 0 回、貸しの後の最初の確保で 1 回、仕事は borrower の thread。空の仕事の貸しの往復 10.9 µs。(o) bug411.html を両方の view で: check が同じ、`_OWN` は貸し 181 回・待ち 35.8 ms（computed-after-change は `_OWN` 51 ms・`_CALLER` 36 ms）。(p) borrower の slips 0 |
| 同 `tsan` | ThreadSanitizer の報告 0、39 checks のうち失敗 2 つは時間の上限（(h) 1757 ms、navigation-wait の caller 802 ms）で、p006 の記録と同じく tsan の遅さ（基点でも (e)・(h)・navigation-wait が同じく落ちる）。基点も今も、lavapipe の unload で TSan が exit 66 で process を止めていた（(i) の途中から先が走らない、基点で確かめた。p008 のせいではない）ので、試験が lavapipe を最後まで読み込んだままにした |
| `sh plan/ws345/tests/host-terminate.sh`・`host-loader-client.sh`・`host-threads-table.sh` | 39/39、23 checks 0 failed、16 checks 0 failed |
| `STALL_MODE=own sh plan/ws345/tests/stall-probe.sh` | 窓の event の最長: idle 0.3、loop 0.3、handler 0.2、sync-xhr-connect 0.1、sync-xhr-read 0.1、docwrite-connect 0.2、async-connect 0.2、resize-big 0.2、decode-big 0.1 ms、over50 は全部 0 |
| bug411 の bench（`bench/bug411.sh` に基点と今の browser、3 回ずつを 3 回、各中央値） | 基点 big-object-reads 953・955・976、grow 65・66・66、computed-after-change 33・33・33、compile 12・12・12 ms。今 954・953・1002、64・64・68、33・33・34、12・12・12 ms。差は揺れの中（bench は `_CALLER` の headless で、貸しは無い） |
| `raster-cost`（基点と今、bug411.html と 6000 個の box の頁、各 2 回） | 基点: 描くだけ 0.82・0.88／17.7・17.5 ms、resize の後の layout と描画 8.5・8.8／332・333 ms。今: 0.95・0.81／17.1・17.5 ms、9.2・8.5／335・322 ms（`_CALLER`、揺れの中） |
| `run-dom-tests.py`（基点と今） | 両方 39/42、落ちた 3 件の出力の diff 無し |
| `run-test262.py --jobs 32`（基点と今の host-js） | 両方 32069/47792、es5 7908/8087、log の diff 無し（suite は p8-uat3 の build/ws074-suites/test262 を読むだけの symlink） |
| `sh plan/ws334/tests/stage1.sh TREE plain`（前 `git archive 57bc8e37f5`、後 今の tree） | 前後とも host-build warning 0・102、host-tests 82 exit 0・17 other（host-exits.txt の diff 無し）、js 21/21、loader 11/11、http 15/15、http-async 19/20（前後同じ）、fetch 38/38、component 83 checks PASS |
| zedBSD の `libbrowser.so`（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk`・`config-rpi5.mk`・`config-visionfive2-keiland.mk`、`BUILD=build/p008-{amd64,rpi5,vf2}`、target `…/dynamic/libbrowser.so`） | 3 つとも exit 0、warning 0（-Werror）、`view/style.o` を含む |
| `sh plan/ws334/tests/cross-build.sh` | exit 1（`cross-build: FAIL`）。失敗は 2 行だけで、win64・winarm64 とも「zip に `share/licenses/libwebp-COPYING.txt` があり `plan/ws334/tests/zip-contents.txt` に無い」（libwebp-compat を Windows の build に足した変更の続き。p008 と関係しない、WS334 の試験の直しが要る、Q1 へ）。他の項目は win64・winarm64 とも PASS、winarm64 も時間切れにならなかった |

### 未実施・残り

- WPT の reftest の見本（`run-wpt-reftests.py` 既定の 100 件）: host に WPT の checkout が無い（main の `build/ws074-suites/wpt` は `/tmp/zedbsd-ws074/...` を指す壊れた symlink）。p008 の変更は `_CALLER` の道では貸しがその場で走るだけなので、DOM の試験・段 1・component・raster の前後が同じだった。WPT を取得して流すかは Q1 の判断。
- `_OWN` の性能: 貸しは 1 回 約 11 µs の往復に加え、計算を別の core で走らせる費用で、変更と読みを繰り返す頁では `_CALLER` より遅い（1000 回で 600 → 701〜757 ms、bug411 の computed-after-change 36 → 51 ms）。窓と描画は止まらない。spin で待つなどの詰めは今はしていない。
- QEMU・実機・Wine での `_OWN`（shell が `_OWN` を使うのは p007）。
