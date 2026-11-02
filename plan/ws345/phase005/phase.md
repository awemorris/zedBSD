<!-- awesome-plan project=zedbsd record=ws345-p005 -->

# ws345-p005 view の front と top の script の thread

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P5 q1980-i01。結果は下）

## 目的

`BROWSER_THREADING_OWN` の view: top の browsing context を `browser-script` の thread に移し、窓の thread は front の口（積む・snapshot を読む）だけを使う（D1・D3、design.md §5.2〜§5.4・§9）。この Phase では style・layout・display list は script の thread（p008 で style の thread へ）、描画は CPU の確定した frame（p006 で GPU と frame の箱）。

## file

- `include/browser/browser.h`（版 5）: `threading`、`frame_wake`、`browser_view_request_image` と callback `image`、`_focus_field` の答えの callback `field`、`browser_view_snapshot_serial`、`browser_view_commit_text_in`（session の番号付き）、`_OWN` で `ENOTSUP` の口の一覧。
- `view/front.c`（新）: mailbox（FIFO、lock と condition、入力の合わせ方、押す入力 1024 件の上限・離す入力は必ず積む）、snapshot（通し番号付き: url・title・can_go・progress・scroll・document の高さ・scroll の範囲・文字入力の状態と session）、通知の queue（1024 件、console の溢れは数の 1 行）と `browser_view_process` での callback。
- `view/view.c`: 今の view を script の thread の中の物にする（front から呼ばれる）。script の thread の loop（mailbox、loader の client、timer の期限の `cond_wait_for`）。
- process に一つの可変の static（design.md §11 R1 の一覧、最初に全部洗い直す）を page・thread の context か atomic に移す。
- 打ち切り（p004）を navigation・destroy に結ぶ（task の番号）。打ち切りの後の `pagehide`・`unload` を 1 s の上限で。戻る・進むで保つ頁は打ち切られた物を保たない。

## 手順

1. front と mailbox、snapshot、通知。`_CALLER` の道は今のまま（front が直に呼ぶ）。
2. script の thread の始めと終わり（`browser_view_create` が始め、`_destroy` が打ち切り → 終わりを積む → join）。
3. 入力の順と合わせ方、routing は top だけ（iframe は p009）。
4. CPU の確定した frame: 描画の更新の後に pixel ではなく display list を確定し、`browser_view_render_pixels` がどの thread からでも最新を描ける（p006 で frame の箱に広げる）。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-threaded-view.c`（窓の代わりの thread と描画の代わりの thread を持つ、probe の形）: (a) timer の script が 5 s 回る間と同期の XHR が connect の掛かる listener を待つ間に、窓の代わりの loop の event の遅れの最長が 20 ms 以下、(b) 押し・key・移動の順が page の listener の見た順と同じ、(c) 移動の合わせ方（連続の移動が 1 つ）、(d) `for (;;) {}` の間の destroy が 1 s 以内に戻る、(e) navigation が無限 loop の頁から 1 s 以内に新しい頁を出す。
- 両方の mode で host-navigation-wait と同じ場面（新しい試験の中で）。
- (f) 押しを 1100 回積んだ後も離す入力が積まれる、(g) focus の後の `field` の答えと session の番号付きの commit_text が古い session の時に捨てられる。
- ThreadSanitizer の variant で (a)〜(e) に race の報告が無い。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。`_CALLER` の回帰が前と同じ。

## 結果（2026-11-02 P5、q1980-i01）

基点 c7d3d9f1bb。branch `agent/p5-ws345`。

### 作った物

- `include/browser/browser.h`（v5 をそのまま広げた、2026-11-02 Q1 の判断）: options に `threading`（`BROWSER_THREADING_CALLER` 0 ／ `BROWSER_THREADING_OWN` 1）・`frame_wake`・`frame_wake_context`、callbacks の後ろに `image`・`field`、新しい口 `browser_view_snapshot_serial`・`browser_view_render_pixels`・`browser_view_request_image`・`browser_view_commit_text_in`。`_OWN` で `ENOTSUP` の口（settle・dump・draw_pixels・prepare・draw・record・image_at）と、何もしない口（set_gpu・release_targets）、-1 の `timeout` を書いた。
- `view/front.h`・`view/front.c`（新）: `browser-script` の thread（64-bit 8 MiB、32-bit 2 MiB の stack）が view を作り（pages の stack base はその thread の frame）、mailbox の FIFO（lock と condition）を取り、各 entry を view の agent の task として、その後に view の round（loader の news・timer・画像）を task として回し、publish する。合わせ方: 続いた移動は最後の一つ、wheel は差を足す、size と color scheme は最後の一つ。押す入力（button・key の down、compose・commit）は 1024 件まで（越えると `ENOBUFS`）、離す入力・focus・命令は必ず積む。snapshot（url・title・history・progress・scroll・文書の高さ・scroll の範囲・大きさ・text の session・caret・値の周り、通し番号）は変わった時だけ番号を進める。通知（redraw は一つに・title・committed・load・console・script_error・image・field）は窓の `browser_view_process` で callback に。console は 1024 件を越えると数えて「N console lines were dropped」の 1 行。`link` は script の thread で program の callback を呼ぶ。script の thread は自分を `_Thread_local` の印で知る（program の thread が書く handle を読まない、tsan で見つけた）。
- `view/view.c`: 全ての public の口の先頭に「窓の thread なら front へ」（`view_front_window`）。`view_make_page` が頁の heap を view の agent に付ける。`view_keep` の `pagehide` は `_OWN` で 1 s の deadline（`vm_agent_set_deadline`）、打ち切られた頁は保たない。新しい口の実装と front の accessor（`view_attach_front`・`view_front_of`・`view_loader`・`view_fill_snapshot`）。
- `vm/`: p004 の task の番号と打ち切りの要求を heap から `struct vm_agent`（vm.h、view の front が持ち、view の頁の heap が共有する。段 B で agent ごと）に移した。deadline（`VM_AGENT_DEADLINE` の印で poll を遅い道へ送り、時計を読む）。`vm_heap_task_*`・`vm_heap_request_termination` は heap の agent への包みとして残す（p004 の試験はそのまま通る）。
- 打ち切り: 窓の thread の navigation（`_load`・`_follow`・`_go`）と `_destroy` が、今走っている task の番号を `vm_agent_request_termination` に書き、`net_loader_client_abort_waited`（新、`net/loader.c`、Q1 了承）で script の thread が待つ request（同期の XHR・parser の script・最初の頁）を起こしてから積む。`_destroy` は ending の印を立てて join（script の thread は残りの entry を走らせずに view を閉じる）。`_stop` は打ち切らない。
- CPU の確定した frame（Q1 の判断、下）: script の thread が redraw の後と大きさの変化で CPU の renderer で絵を描き（`browser_view_draw_pixels`）、参照の数を持つ絵にして lock の下で入れ替え、`frame_wake` を呼ぶ。`browser_view_render_pixels` はどの thread からでも最新の絵を写す（覆わない所は白）。`_CALLER` の view は今のまま（`render_pixels` は `draw_pixels`）。shell の GPU の道は p007 まで変えない。
- R1（process に一つの可変の static、洗い直した）: 計算の途中の値（`bind/computed.c` の `computed_pseudo`・`_name`・`computed_zoom`、`css/cascade.c` の `cascade_animation_suppressed`、`layout/position.c` の `position_remember`、`page/svg.c` の `svg_view_*`、`net/http.c` の `http_fetch_kind`、`js/builtin_math.c` の `math_random_state`）を `_Thread_local` に、数（`image/decode.c` の `img_next_serial`、`bind/prelude.c` の `top_layer_order`、`net/http.c` の dump の counter）を atomic に。`net/tls-static.c` の callback は tls.c の lock の下で書かれるので残した。Windows の build は `_tls_index`（mingw-w64 の CRT の TLS の index）を import するので `plan/ws334/tests/allowed-imports.txt` に足した。
- 変えた試験の file: `plan/ws345/tests/host-threaded-view.c`・`.sh`（新）、`plan/ws345/tests/raster-cost.c`（新、計測の probe）、`plan/ws345/tests/stall-probe.c`・`.sh`（`own` の mode、`STALL_MODE=own`）、`plan/ws334/tests/allowed-imports.txt`（`_tls_index`）。

### 試験（host、2026-11-02）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan` | exit 0、warning 0、102 built |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | 18 checks、0 failed。(a) timer の script 5 s の間の窓の呼び出しの最長 0.06 ms、同期の XHR が connect を待つ間 6.8〜7.0 ms、描画の代わりの thread が絵を写した。(b) 押し・key・wheel が listener に順のまま（mousedown, keydown a, keyup a, mouseup, click, wheel, keydown b, keyup b）。(c) 忙しい間の 500 回の移動が 1 entry。(f) 押し 1100 回で 76 回 `ENOBUFS`、離すは 0。(d) `for (;;) {}` の間の destroy 2.9〜8.1 ms。(e) 無限 loop の頁からの load が 59〜63 ms で新しい頁。(g) `field` の答え（error 0）、古い session の commit は捨て（値 "x"）、今の session は入る（"xzz"）。(h) 終わらない `pagehide` の handler が 1 s で止まり新しい頁（1058 ms）。navigation-wait（host-navigation-wait の場面、遅い sheet 1.2 s）: caller の最長 77 ms（上限 400）、own 0.5 ms（上限 20）、両方で前の頁が sheet の後まで残り（77〜85 ms 後に切り替え）、新しい頁が sheet 付き（高さ 5024） |
| 同 `tsan` | ThreadSanitizer の報告 0（最初の run で 6 件: script の thread が program の thread の書く handle を読む race、上の `_Thread_local` の印で直した）。時間の上限の 3 つ（(a) の XHR 26.5 ms、navigation-wait の caller 740 ms・own 21.5 ms）は tsan の遅さで超える（plain では通る） |
| `sh plan/ws345/tests/host-terminate.sh plain`・`tsan`（p004、agent に移した後） | 39/39、tsan の報告 0 |
| `sh plan/ws345/tests/stall-probe.sh`（6 s、caller ／ `STALL_MODE=own`） | caller: idle 54.5、loop 3052、handler 994.5 ms、sync-xhr-connect は終わりまで止まったまま。own（3 回）: idle 0.2、loop 0.2〜1.0、handler 0.1、sync-xhr-connect 2.8、sync-xhr-read 0.2〜3.3、docwrite-connect 0.2〜3.3、resize-big 0.1〜0.3、decode-big 0.2〜7.1 ms（over50 は 0。最初の 1 回だけ loop で 56.6 ms が 2 件、host の負荷の時） |
| 絵の値段（`raster-cost`、800 × 600、11 回の中央値） | bug411.html（bench の頁）: 描くだけ 0.83 ms、resize の後の layout と描画 8.1 ms。6000 個の box の頁: 描くだけ 16.6 ms、resize の後 318 ms |
| `sh plan/ws334/tests/stage1.sh TREE plain`（前 `git archive c7d3d9f1bb`、後 0ab5211c5c の前の tree） | 前後とも host-build warning 0・102、host-tests 82 exit 0・17 other（host-exits の diff 無し）、js 19/19、loader 11/11、http 15/15、http-async 19/20（同じ async-sheets）、fetch 38/38、component 83 checks PASS |
| `sh plan/ws334/tests/cross-build.sh` | 1 回目 FAIL（libbrowser.a の `_tls_index` が allowed-imports に無い）。足して 2 回目 PASS（win64・winarm64、Wine の確かめを含む、compat と libtruetype の warning 0） |
| `sh plan/ws074/tests/bench/bug411.sh 前 後`（中央値 ms） | 前 big-object-reads 1037、compile-many-names 13、computed-after-change 35、grow-with-lookups 69 ／ 後 1038、13、36、71（雑音の幅） |

### Q1 の判断（2026-11-02）

- step 4 の暫定: p005 では script の thread が redraw の後に CPU の renderer で絵を描き、参照の数を持つ絵を `browser_view_render_pixels` が小さな lock の下で写す（display list の確定と描画の thread の renderer は p006）。条件: `_OWN` だけ、`_CALLER` と shell の今の GPU の道は p007 まで変えない、重い頁の絵の値段を記録（上の表）。
- v5 をそのまま広げる（v5 はまだ出していない）。`net_loader_client_abort_waited` の追加を了承。

### design.md への提案（P8 の文書、Q1 経由）

- §6.1 の始めに: 「p005 の暫定: `_OWN` の view では script の thread が描画の更新の後に CPU の renderer で view の大きさの絵を描き、参照の数を持つ絵を確定として置く。`browser_view_render_pixels` はどの thread からもその最新を写す。p006 で display list の確定と描画の thread の renderer に替える（重い頁の値段: 6000 個の box で描くだけ 16.6 ms、resize の後 318 ms、phase005 の結果）。」
- §9 の打ち切り: task の番号と要求は heap でなく `struct vm_agent`（view の front が持ち、頁の heap が共有）にある事と、navigation・destroy が `net_loader_client_abort_waited` で待つ request を起こす事、`pagehide` の 1 s は agent の deadline である事。

### 未実施・残り

- iframe の入力の振り分けを libbrowser の別の thread で行う事（J6）: この Phase では top の script の thread の今の当たり判定のまま（iframe の thread は p009）。
- QEMU・実機・Wine での `_OWN`（shell が `_OWN` を使うのは p007）。zedBSD の target の build（`_Thread_local` を含む libbrowser.so）は未確かめ。
- asan の variant。
