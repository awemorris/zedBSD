<!-- awesome-plan project=zedbsd record=ws345-p010 -->

# ws345-p010 origin ごとの agent（段 B）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: in-progress（2026-11-14 P3 q2090-i01: 引き継ぎの (1)〜(6)・(8) を済ませた。(7) WPT は p011 へ。cleared の判定は Q1。下の「結果（2026-11-14 P3）」。前: 2026-11-02 P5 q1995-i01 が段 B の核と host の試験まで）

## 目的

cross-origin の iframe を別の agent（別の heap と lock）にし、並べて走らせる（D3、design.md §4.1・§8）。

## file

- `vm/`（heap の複数）・`bind/frame.c`・`bind/window.c`（遠くの WindowProxy・Location）・`bind/cloning.c`（heap の外の bytes の形の structured clone）・`page/frame-load.c`・`view/front.c`（agent ごとの打ち切り）。

- p009 から移した物（2026-11-02 Q1 の判断）: iframe を自分の frame として layout・確定する事（親の確定した list の「frame の item」）、`page/frame-load.c`・`paint/list.c`・`paint/scene.c`・CPU と Vulkan の renderer・`view/front.c` の frame の箱。設計の覚え（P5）:
  - 今（p009 の段 A）は top の script の thread が `front_publish` で頁を確定する時、親の paint（`list_frame` → `page_frame_paint` → `frame_make`）が子の文書を agent の下で layout・paint し、子の list を親の list に写す。iframe の thread の 1 周の後は必ず top が起こされて確定し直すので、確定は自足し正しい。段 B では親の thread が cross-origin の子の heap を読めないので、この形は使えない。
  - 形: iframe の thread がその 1 周の終わりに（子の文書の世代か、親が最後に決めた枠の大きさ・zoom が変わった時）自分の文書を `frame_tree`・`page_lend_paint` で layout・paint し、view の「frame の箱」（frame の id → 最新の確定、lock の下、参照の数は atomic）に置いて `frame_wake` を呼ぶ。親の `list_frame` は子の item を写さず、新しい種類の item（frame の id・枠・clip・canvas の色）を一つ置く。枠の大きさと zoom は親の layout が決めて箱に書き（今の `frame_size_walk` の値）、子の thread を起こす。
  - 描画の thread は frame の item に出会うと、その id の最新の確定を箱から引いて（無ければ canvas の色）中の item を枠の位置へずらして描く（CPU は `paint/raster`、GPU は `paint/vulkan.c` の記録の再帰。深さは `FRAME_MAX_DEPTH` で抑える）。scene の画像の hold と font の bytes は子の確定が自分で持つ。
  - 入力の routing（design.md §5.3、J6）も同じ確定の iframe の枠の表で行う（p009 では top の thread が今のとおり子の文書に event を出す。段 A では同じ agent なので意味は同じ）。

## 試験

- `plan/ws345/tests/host-frames.c` に足す: cross-origin の iframe（試験の server の 2 つの port）の無限 loop の間に top の timer が走る、postMessage が両方向に届き値が同じ（Array・Map・Date・ArrayBuffer の transfer）、`MessagePort` の transfer は `DataCloneError`、cross-origin の `location` の代入が子を navigation する、`frames.length`・`parent`・`top` が正しい、cross-origin の DOM の access が `SecurityError`。
- WPT の `html/webappapis/` と `html/browsers/` の postMessage・cross-origin の部分集合（Q1 と範囲を決める）。ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。
- cross-origin の iframe の timer が無限 loop の間も、top の文書の script（timer・入力の listener）が走り続ける（2026-11-02 Q1 の判断で p009 の受け入れから移した。段 A の p009 では同じ agent の iframe の task が agent を持つので top の script は待ち、窓と描画だけが止まらない）。

## 結果（2026-11-02 P5 q1995-i01、ラップアップ）

基点 main 6c1af97bc1。commit a6479c00c5（段 B の核と試験）と、その後のラップアップの commit（下の「引き継ぎ」に SHA）。

### 作った物

- `view/remote.c`・`view/remote.h`（新）: origin の違う iframe の「他の agent の frame」。`_OWN` の view で、iframe の最終の URL が親と違う web origin の時（両方が web origin、sandbox で opaque でない）に、その iframe は自分の `struct page`（自分の heap と `vm_agent`）を自分の `browser-script` の thread と loader の client で持つ。親の要素は C の pointer（`dom_element.frame_remote`）だけを持ち、heap を越える cell は無い。番号（view で再利用しない）で引く。親の agent への用（親・top への message、自分・親・top の navigation、load の終わり）は queue に積み、親の文書を走らせる thread（top は `front_loop` の新しい task `view_frames_deliver`、iframe の thread は `frames_round`）が task として配る。frame の数は p009 の上限（top を含め 64、32-bit は 16）に数える。要素が外れる・頁が back/forward に保たれる・view が終わる時に、その task の打ち切り・待つ request の起こし・thread の join（頁はその thread で壊す）。保った頁を戻すと iframe は src から読み込み直す。応答しない頁の問い（p014）用に `view_remotes_busy`（走っている task の frame の番号と task の番号）と `view_remotes_stop`（番号で打ち切り）を出した（p014 の `browser_view_script_busy`・`_stop_script` への結び付けは後に merge する側、Q1 の指示）。
- `bind/remote.c`（新）: 他の agent の window の proxy（`postMessage`・`closed`・`length`・`window`・`self`・`frames`・`parent`・`top`・`opener`・`location`（`href` の代入と `replace`）・`focus`・`blur`・`close`、他は `SecurityError`）、message の tape（structured clone を heap の外の平らな項目の列に: primitive・Array・plain object・Map・Set・Date・RegExp・primitive の wrapper・Error・ArrayBuffer と view・参照。transfer の ArrayBuffer は copy と detach、MessagePort などは `DataCloneError`）、受け手の task での `MessageEvent`（trusted、origin と source）。proxy と tape は page の言語で書き、要る window で初めて走らせる。`js/builtin_typed.c` に `js_typed_detach`・`_detached`。
- `page/frame-load.c`: 他の origin の応答を `remote_load` に渡す（前の文書の pagehide・unload と window の退役）、同じ origin に戻る応答で他の agent の frame を閉じる、`page_frame_remote`・`page_remote_loading`・`_loaded`（親の window の load は他の agent の frame の load を待つ）・`page_remote_navigate`、suspend で閉じて読み込み直しの印。`bind/frame.c`: `contentWindow` は proxy、`contentDocument` は null、他の agent の frame の top-level window の `parent`・`top` は proxy、`bind_frame_foreign`。`bind/frame-resource.c`: `bind_frame_retire`、子の window に `host.remote` を引き継ぐ。
- 描画: `paint/paint.h` の `PAINT_FRAME`（frame の番号と content box、clip の中）、`paint/list.c` の `list_frame` が他の agent の frame にこの item を置く、`paint/scene.c` の `paint_frames_resolve`（描く時に番号で最新の scene を引き、枠の位置へずらして写す。canvas の色、web font の face を空いている web face に移す、深さ 8 まで）、`view/view.c` の CPU と GPU の描画がそれで描き、GPU は次の描画まで保つ。renderer（`paint/software.c`・`paint/vulkan.c`）は解かれない frame の item を描かない。他の agent の frame は自分の thread で layout・paint・scene を作り frame の箱に置いて `frame_wake`（`view_front_wake_drawing`）を呼ぶ。top は確定し直さない。
- 入力（J6）: `view/view.c` の pointer の移動・押し・離し・wheel を、top の script の thread で最新の確定した frame の frame の item の矩形で振り分ける（他の agent の frame の viewport の座標に直してその inbox へ）。押した frame が key を受ける。
- `view/front.c`: `front_loop` に配る task を一つ、`view_front_wake_drawing`。`front_note`・`front_publish`・`view_front_process` は変えていない。
- `bind/environment.c`: window の `length` に 0 を置くのをやめた（prelude の frame を数える getter を隠していて、`frames.length` が常に 0 だった）。

### 試験（host、2026-11-02）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan`（a6479c00c5 の時点） | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-frames.sh plain`（(k)〜(r) を足した） | 29 checks、0 failed: (k) 他の origin の iframe の無限 loop は自分の thread で 1000 ms のうち 1000 ms、その間 top の 50 ms の interval が 1 s に 20 回、窓の呼び出し最長 0.08 ms。(l) 同じ origin の iframe は top の agent のまま。(m) postMessage の往復（Array・object・Map・Set・Date・RegExp・-0・参照・ArrayBuffer の transfer と送り手の byteLength 0、source と origin、MessagePort は DataCloneError）。(n) frames.length・parent・top・closed・SecurityError を両側から。(o) 親が子の location を変える、子が top.location を変える、同じ origin に戻ると top の agent に戻る。(p) CPU・GPU とも `_CALLER` と画素で同じ、子だけの変化は top の確定の番号が変わらずに描かれる。(q) 押しと key は子の文書へ（top の listener は 0 回）。(r) 要素を外すと他の agent の frame が終わる、保った頁で 0 になり戻ると 1 |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | 40 checks、0 failed |
| `sh plan/ws345/tests/host-frames.sh tsan`（a6479c00c5 の tsan の build） | 29 checks、0 failed。ThreadSanitizer の報告 3（exit 66）: 二つの thread の `page_create` が同時に走る時の既存の process に一つの書き込み 2 件（`page/media.c` の `page_media_init` が毎回 `media_set_log` で libmedia の global を書く）と、`js/builtin_date.c` の `wb_os_time_zone_start`（glibc の `tzset` の中、計装されない libc の lock の誤報の可能性）。どちらも段 B で top と他の agent の frame の頁が同時に作られて表に出た（複数の view でも起こり得る） |
| zedBSD の `libbrowser.so`（amd64・rpi5・vf2） | 1 回目は clang の `-Wunused-but-set-global`（`view/remote.c` の使わない thread-local）で 3 つとも失敗。直して再 build: 3 つとも exit 0・warning 0（`build/p010-{amd64,rpi5,vf2}/dynamic/libbrowser.so`、`view/remote.o`・`bind/remote.o` を含む） |

変えた試験の file: `plan/ws345/tests/host-frames.c`・`host-frames.sh`（(k)〜(r)、二つ目の port、GPU の読み戻し、lavapipe の tsan の抑え）。

### 見つけた事（この Phase で直していない）

- block-level・positioned（absolute など）の iframe は描かれない（`_CALLER` でも）: `list_frame` は inline の置換要素の fragment（`list_replaced`）からしか呼ばれない。他の agent の frame の item も同じ所から置くので、その iframe には item が無く入力も振り分けられない。WS074 側の不具合の候補（Bug Board へ、Q1 の判断）。

### 制限（Future Work の候補、Q1 が認めた物）

- 他の agent の frame の中の iframe はその frame の thread と heap のまま（段 A の形。その中の他の origin の孫も同じ heap）。
- 他の agent の frame は zoom 1 だけ（親の iframe の zoom・opacity・transform は子の item に掛からない）。
- iframe ごとの history は無い（他の agent の frame の navigation は親の frame-load で入れ替える）。
- `parent.parent`・`top` の向こうの窓・`frames[i]` の index は近似（他の agent の側からは top の proxy、他の agent の proxy の index は無し）。`parent.length` は読み込みの時の数。
- 他の agent の frame の text 入力（compose・commit_text）・hover の mouseout・scroll は振り分けない（pointer・wheel・key だけ）。

## 引き継ぎ（2026-11-02 23:56、P5 ラップアップ）

- 未実施（次の担当がこの順に）: (1) 最後の commit の tree で `host-build.sh plain`・`tsan`（warning 0）と `host-frames.sh plain`・`tsan`（29 checks、tsan の報告 0）、`host-threaded-view.sh tsan`。(2) zedBSD の `libbrowser.so` 3 target は 132803cab2 の remote.c で済み（最後の commit で変えたらやり直し）。(3) `host-terminate.sh`・`host-loader-client.sh`・`host-threads-table.sh`、`STALL_MODE=own sh plan/ws345/tests/stall-probe.sh`、bug411 の bench、`run-dom-tests.py`・`run-cross-origin-tests.py`・`run-test262.py` を基点（`build/tmp/p010/base` は `git archive 6c1af97bc1`、stage1 を走らせ始めてある）と比べる。`window.length` の変更で DOM の試験の出力が変わらないかを特に見る。(4) `sh plan/ws334/tests/stage1.sh TREE plain` を基点と最後の commit の archive で、`sh plan/ws334/tests/cross-build.sh`（`view/remote.c`・`bind/remote.c` を `sources.mk` に足した。Windows の build の一覧が別にあれば足す）。(5) tsan の報告 3 件を直す: `page_media_init` の `media_set_log` を process に一度（`wb_once_run`）、`wb_os_time_zone_start` も一度にするか誤報なら抑えの理由を書く。直した後に host-frames tsan で報告 0 を確かめる。(6) p014 の merge の後に `browser_view_script_busy`・`_stop_script` を `view_remotes_busy`・`_stop` にも広げる（Q1 の指示: 一番長く走る task を全 agent から、止めるのはその task の agent）。(7) WPT の postMessage・cross-origin の部分集合（p011 で suite を取得する時に）。(8) design.md §4.1・§6.1・§8 に上の作りの覚えを入れる（未記入）。
- 消してほしい物（Q1 へ）: `build/tmp-p010-hostbuild-plain.log`（最初の build の log、置き場所の誤り）。`build/tmp/p010/` は試験の一時物。

## 結果（2026-11-14 P3 q2090-i01、引き継ぎの残り）

基点 main c5b8a10d24（p010・p014 の branch は merge 済み: `view/remote.c`・`bind/remote.c`、`view_remotes_busy`・`_stop`、p014 の `browser_view_script_busy`・`_stop_script` が main にある事を確かめた）。commit 7073d0c19c（libbrowser と host-frames）、記録と試験の script の commit は ws.md の Resume point。

### 今との乖離（2026-11-14 に確かめた）

- build: `config/ci/*.mk`・`ZEDBSD_CONFIG`・`BUILD=build/p010-*` の形は古い。今は `make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` → `sysroot` → `build/<機種>/packages/browser/.stamp`（出力は `build/<機種>/dynamic/libbrowser.so`・`build/<機種>/packages/libbrowser/obj/...`）。上の 2026-11-02 の表の command は当時の形のまま残す。
- host の試験の道具（`plan/ws074/tests/host-build.sh`・`plan/ws345/tests/host-*.sh`・`stall-probe.sh`・`plan/ws334/tests/stage1.sh`・`cross-build.sh`・`shell-wine.sh`）は今の tree でそのまま動いた。`shell-wine.sh`・`resize-wine.sh` の make の `-j8` は host の負荷の上限（4 thread）に合わせ `-j4` にした。
- `shell-guest.sh` の image の config `plan/ws074/tests/config-amd64-browser.mk` は `ZEDBSD_USER_PROGRAMS` で browser を足すが、今の build はこの変数を読まない（`mk/` に無い）。image に browser が入らない見込み（image は作っておらず未確認）。p011 の QEMU の前に WS345 の `tests/` に `CONFIG_USERLAND_PROGRAMS` の形の config を置いて替える。
- test262 の suite（`build/ws074-suites/test262`）は worktree に無い（build/ は空から）。p011 で suite を取得する時に流す。

### 直した物

- (5) ThreadSanitizer の報告 3 件: `page/media.c` の `page_media_init` は libmedia の log の設定（process に一つの global）を `wb_once_run` で process に一度にした。`base/os-time-posix.c` の `wb_os_time_zone_start`（`tzset`）は static mutex（`wb_static_mutex`）で一度に一つの thread にした。glibc の `tzset` は自分の lock の下にあり報告は ThreadSanitizer から見えない lock の誤報だが、C library が `tzset` 同士を守る事は保証されないので（zedBSD の libc を含め）直列にした。realm ごとに zone を読み直す今の意味は保つ。
- (6) 応答しない頁の問いを他の agent の frame に広げた（Q1 の指示: 一番長く走る task を全 agent から、止めるのはその task の agent）:
  - `vm/heap.c`・`vm/vm.h`: task の番号を agent ごとの数えから process で一つの atomic の数えにした（`vm_agent.task_last` を除いた）。二つの agent の task が同じ番号を持たないので、番号で agent が決まる。
  - `view/remote.c`・`remote.h`: `view_remotes_busy` は一番長く走る frame の task の番号と時間、`view_remotes_stop(remotes, task)` はその番号の task を走らせる frame の agent に打ち切りを書きその client の待ちを起こす（0・ESRCH）。
  - `view/front.c`: `view_front_script_busy` は view の agent と他の agent の frame の長い方。`view_front_stop_script` は view の agent の task ならそれと同じ agent の iframe の client の待ちだけを起こし（新しい `view_frames_abort_agent_waited`）、そうでなければ他の agent の frame に回す。前は top の task を止めると `view_frames_abort_waited` が他の agent の frame の task まで打ち切っていた。navigation・destroy は今のとおり全 agent を止める。
  - `include/browser/browser.h`・`view/view.c` の説明を合わせた。
- (8) design.md §4.1・§6.1・§8・§9 に段 B の作りと上の busy・stop を入れた。

### 試験（host、2026-11-14、7073d0c19c の tree）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan` | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-frames.sh plain`・`tsan` | 両方 `host-frames: 33 checks, 0 failed`。tsan の報告 0（exit 0）。新しい (k) の 4 check: 他の agent の frame の loop が view の busy（1003 ms）、その番号の stop で 6 ms で終わり frame は残る、その後 1 s に top の interval 20 回、終わった番号の stop は ESRCH |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | `55 checks, 0 failed` |
| `sh plan/ws345/tests/host-threaded-view.sh tsan` | 55 checks、ThreadSanitizer の報告 0、失敗 2 は p014 の記録と同じ tsan の遅さの時間の上限（(h) 1694 ms、caller の navigation-wait 732 ms） |
| `host-terminate.sh`・`host-loader-client.sh`・`host-threads-table.sh`・`host-prompt-rule.sh` | `39/39 passed`・`23 checks, 0 failed`・`16 checks, 0 failed`・`13 checks, 0 failed` |
| `STALL_MODE=own sh plan/ws345/tests/stall-probe.sh` | 9 場面とも窓の event の最長 0.1〜4.0 ms（sync-xhr-connect 4.0）、over50・over100・over1000 0 |
| `sh plan/ws334/tests/stage1.sh TREE plain`（基点 c5b8a10d24 と 7073d0c19c の `git archive`） | 前後で同じ: host-build warning 0・built 104、host-* 84 exit 0・17 other（exit の一覧が同じ）、js 21/21、loader 11/11、http 15/15、http-async 19/20（両方 `async-sheets` が FAIL、前からの物）、fetch 38/38、component 83 checks PASS |
| `run-dom-tests.py`・`run-cross-origin-tests.py`（基点と後の browser） | 両方 `dom-tests 39/42`（同じ 3 件 computed・exception・performance-observer が FAIL）、`cross-origin-tests: 21/21` |
| `sh plan/ws074/tests/bench/bug411.sh`（基点と後） | big-object-reads 1038 / 1044 ms、compile-many-names 12 / 13、computed-after-change 2 / 2、grow-with-lookups 70 / 71（check の値が同じ） |
| `make -j4 CONFIGMK=config/config.{x86_64,rpi5,vf2}.mk build/<機種>/packages/browser/.stamp` | 3 つとも exit 0、warning 0（`dynamic/libbrowser.so`、`view/remote.o`・`bind/remote.o` を含む） |
| `BUILD_JOBS=4 sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS` |
| `sh plan/ws345/tests/shell-wine.sh build/tmp/p3/shell-wine`（Wine・lavapipe、全 case） | `23 passed, 0 failed`、`shell-wine: PASS`。prompt・prompt-cpu の問いは 8〜9 s、Stop で tick 1→8・spin end 0、窓の待ち最長 11.1〜32.3 ms・over_100ms 0（2 回目の起動の modal の大きさの loop の約 3 s は判定しない記録のとおり） |

### 未実施・残り

- (7) WPT の postMessage・cross-origin の部分集合と test262 は suite が無く未実施（p011 で取得して流す）。zedBSD の QEMU（shell-guest）は image の config の乖離（上）で今回は流していない。
- 消してほしい物（Q1 へ）: 2026-11-02 の `build/tmp-p010-hostbuild-plain.log`・`build/tmp/p010/` は今の worktree には無い（別の worktree の物）。今回の一時物は `build/tmp/p3/`（基点と後の archive、約 1.6 GB）と worktree 直下の誤った出力 `plain`（symlink）・`plain.run.3PIMxr/`。
