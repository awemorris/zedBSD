<!-- awesome-plan project=zedbsd record=ws345-p009 -->

# ws345-p009 iframe ごとの script の thread（段 A）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: in-progress（2026-11-02 P5 q1993-i01: 実装と試験は終わり。iframe の frame の item と自分の確定は Q1 の判断で p010 へ移した。cleared の判定は Q1。結果は下）

## 目的

iframe（入れ子を含む）ごとに `browser-script` の thread を持たせる（D3、design.md §4.1・§5.3・§8）。段 A は全 frame が一つの agent（一つの heap と lock）。

## file

- `bind/frame.c`・`page/frame-load.c`・`page/page.h`・`bind/timer.c`: frame ごとの task の queue と thread、agent の lock、frame の木（page の lock）、frame の上限 64（32-bit の target は 16、J4）。page に一つの `fetches`・`scripts`・timer（`timer_run_children`）を frame ごとに分ける。frame ごとの loader の client。
- `vm/realm.c`: microtask の checkpoint を agent に一つ（lock の持ち主が agent の全 realm を空に）。
- `view/view.c`: 戻る・進むで保つ頁の iframe の thread を終えて join、戻す時に作り直す（作れなければ読み込み直す）。
- `vm/heap.c`: lock を取った thread の stack の底（R6）。
- `view/front.c`: 入力の routing（確定の iframe の枠の表で iframe の mailbox へ、J6 の答え次第）。
- `paint/`: iframe の frame の item（p006）を各 frame の確定に。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-frames.c`: iframe の timer が top と別の thread（名前と thread の id）で走る、同じ origin の親子の同期の access（`parent.document`・関数の呼び出し）が今と同じ値、iframe の無限 loop の間も窓の代わりの loop の遅れが 20 ms 以下、iframe を外すと 1 s 以内にその thread が join される、65 個目の iframe は `contentWindow` が `null`、frame A が積んだ frame B の realm の promise の反応が A の checkpoint で走る、iframe の同期の XHR が top の thread を待たずに終わる（指摘 2 の deadlock が無い）、保った頁の iframe の thread が join され戻すと作り直される、thread の `start` が失敗する表で iframe が browsing context を持たず親が続く。
- 既存の iframe の試験（host の frame の試験、DOM の試験）が前と同じ。ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。

## 結果（2026-11-02 P5 q1993-i01）

基点 main 379cb83d0f。commit de1bf6719b・7e032ac99e、main 554e305b09（P8 の p007）を merge 34b40d7b3c、9dcc7d3f5c（collectible な window の request の条件）。

### 作った物

- `view/frames.c`・`view/frames.h`（新）: iframe ごとの `browser-script` の thread（stack 8 MiB／2 MiB）と自分の loader の client、agent の lock（top が待つ時は top が先）、数の上限（view に top を含め 64、32-bit は 16）、外す時の join（自分を外した iframe は次に top が join）、止まった iframe の client は request が無くなるまで top が dispatch、保った頁の suspend と resume。design.md §4.1・§7・§9 に p009 の実装の覚え。
- `view/front.c`: `struct view_front` に `frames`、`front_run`（frames を作り、view の作成と destroy の間 agent を持つ）、`front_loop`（mailbox の entry・`browser_view_process`・`front_publish`・次の待ちの期限の計算の間 agent を持つ）、`front_navigation`（iframe の client の待ちも起こす）、`view_front_frames`。`front_note`・`front_publish`・`view_front_process` は変えていない。
- `view/view.c`: `_OWN` の頁に frame の calls、`view_keep` の suspend、`view_restore` の resume（作れなければ読み込み直す）、destroy で loader の前に iframe の client を閉じる、`view_page_time`。
- `bind/`: `bind_host.frame_attach`・`frame_detach`、thread の runner（`bind_set_thread_runner`・`bind_thread_runner`・`bind_element_runner`・`bind_window_runner`）、`frame_create` と `frame_remove_subtree` の attach・detach、`dom_element.frame_thread`・`frame_refused`（`dom/dom.h`）、timer は別の thread の子を数えず回さない（`bind/timer.c`）、`_OWN` で iframe の window に親の `request`・`request_cancel`・`fetch_sync`（`bind/frame-resource.c`）、collectible な window は `request_cancel` がある時だけ `request` を持て `bind_window_detach` で取り消す（`bind/window.c`・`bind/prelude.c` の `bind_requests_cancel`、外れた window の送りは失敗）。
- `page/`: `page_set_frame_calls`・`page_frame_attach`・`_detach`・`page_frames_suspend`・`_resume`（`page/frame-load.c`）、別の thread の要素の読み込みは activate せずその thread を起こす、top の `load` の再開と `script_run_ready` は top の thread だけ、`page_set_thread_client`・`page_client`（`page/network.c`）で取得は全部呼んだ thread の client（frame-load・media・sheets・fonts・images・script）、`page_wait_sheets` は呼んだ thread の client の sheet だけ待つ（`page_sheets_blocking_for`）。
- `vm/heap.c`: `vm_heap_stack_base`（iframe の thread が 1 周の間 stack の底を替えて戻す）。

### 試験（host、2026-11-02）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan`（merge の後） | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-frames.sh plain`（新） | 15 checks、0 failed: (a) iframe の timer の 1.5 s の spin の CPU は iframe の thread に 1500 ms・top の thread 0 ms、その間の窓の呼び出しの最長 0.02 ms、spin の後 600 ms に top の interval が 13 回。(b) 同じ origin の親子の access（`parent.document.title`・関数の呼び合い・`w.parent === window`）と (c) postMessage の往復が `_CALLER` と同じ。(d) iframe の無限 loop の間の窓の呼び出し最長 0.03 ms、そこからの load が 80 ms で出る。(e) listener が外した iframe の thread が 0 ms で join。(f) 64 個の iframe で 63 個目まで window があり 64 個目は `null`、読み込み 6247 ms（`_CALLER` 3525 ms）、窓の呼び出し最長 0.08 ms。(g) 親の realm に積んだ promise の反応が iframe の thread の checkpoint で走る（CPU 800 ms が iframe の thread）。(h) iframe の同期の XHR（300 ms 後に来る応答）と fetch()。(i) 保った頁の iframe の thread が離れると 0、戻ると 1 で interval が続く。(j) thread が始まらない時 iframe は window を持たず親が続く |
| 同 `tsan` | 15 checks、0 failed、ThreadSanitizer の報告 0（(f) は tsan で 50 s、`_CALLER` 51 s） |
| `sh plan/ws345/tests/host-threaded-view.sh plain`（merge の後） | 40 checks、0 failed |
| 同 `tsan` | ThreadSanitizer の報告 0。40 checks のうち落ちた 3 つは時間の上限（(e) 1071 ms、(h) 1757 ms、navigation-wait の caller 737 ms）で、p006・p008 の記録と同じ tsan の遅さ |
| `host-terminate.sh`・`host-loader-client.sh`・`host-threads-table.sh` | 39/39、23 checks 0 failed、16 checks 0 failed |
| `STALL_MODE=own sh plan/ws345/tests/stall-probe.sh` | 窓の event の最長: 全 scenario 0.1〜0.2 ms、over50 は全部 0 |
| `run-dom-tests.py`（基点 554e305b09 と今） | 両方 39/42、出力の diff 無し |
| `run-cross-origin-tests.py`（同） | 両方 21/21 |
| `run-frame-load-tests.py`（同） | 両方 exit 2（runner が渡す引数を今の browser が受けない「more than one page given」。基点も同じで p009 と関係しない） |
| `run-test262.py --jobs 32`（同、host-js） | 両方 32069/47792、es5 7908/8087、log の diff 無し |
| bug411 の bench（`bench/bug411.sh` 基点と今、各中央値） | 基点 big-object-reads 1039・grow 70・computed-after-change 35・compile 13 ms、今 965・65・33・12 ms（揺れの中） |
| `raster-cost`（基点と今、bug411.html と 6000 個の box の頁、各 2 回） | 基点: 描くだけ 0.78・0.93／17.9・17.9 ms、resize の後の layout と描画 10.3・9.2／333・329 ms。今: 0.91・0.83／18.4・16.8 ms、8.7・10.8／337・342 ms（`_CALLER`、揺れの中） |
| `sh plan/ws334/tests/stage1.sh TREE plain`（前 `git archive 554e305b09`、後 `git archive 9dcc7d3f5c`） | 前後とも host-build warning 0・102、host-tests 82 exit 0・17 other（host-exits.txt の diff 無し）、js 21/21、loader 11/11、http 15/15、http-async 19/20（前後同じ）、fetch 38/38、component 83 checks PASS。merge の前の 1 回（379cb83d0f 対 de1bf6719b）で host-context-ownership が 2 になった（collectible な window が request を持てない、の試験）ので、`request_cancel` がある時だけ持てる形に直した（9dcc7d3f5c） |
| zedBSD の `libbrowser.so`（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk`・`config-rpi5.mk`・`config-visionfive2-keiland.mk`、`BUILD=build/p009-{amd64,rpi5,vf2}`、target `…/dynamic/libbrowser.so`、merge と直しの後） | 3 つとも exit 0、warning 0、`view/frames.o` を含む |
| `sh plan/ws334/tests/cross-build.sh`（merge の後） | `cross-build: PASS`、exit 0 |

変えた試験の file: `plan/ws345/tests/host-frames.c`・`host-frames.sh`（新）。`plan/ws345/phase010/phase.md` に p009 から移した物を書いた。

### Q1 の判断（この Phase）

- 段 A では同じ agent の iframe の task が agent を持つので、その間 top の script は待つ（どの browser も同じ origin の frame では同じ）。p009 の受け入れは窓と描画が止まらない事・loop が iframe の thread で走る事・task が終われば top が続く事。「cross-origin の iframe の loop の間も top の script が走る」は p010 の受け入れへ移した。
- iframe を自分の frame として layout・確定する事（frame の item）は p010 へ移した（段 A では top の確定が agent の下で子を layout し、iframe の 1 周ごとに top が確定し直すので確定は正しい。必要になるのは親が子の heap を読めない段 B）。

### 未実施・残り

- 入力の routing（確定の iframe の枠の表で iframe の mailbox へ、J6）: 入れていない。iframe の中の要素への pointer・key の event は今のとおり top の thread が子の文書に出す（段 A では同じ agent なので順と意味は同じ）。p010 の frame の item と一緒に（phase010 に書いた）。
- `page->fetches`・`scripts` の表を frame ごとに分ける事はしていない。表は page に一つのまま、callback は要求した thread の client で走り、page の script と top の load は top の thread だけが走らせる。
- 64 個の iframe の頁の読み込みは `_OWN` で `_CALLER` の約 1.8 倍（6.2 s 対 3.5 s）。各 iframe の thread が自分の 1 周ごとに頁の全 iframe を scan して他の iframe を起こす事と agent の受け渡しの費用。窓は止まらない。詰めは p011 の計測の後。
- QEMU・実機・Wine での iframe の thread（shell の `_OWN` は p007 で main にある）。WPT は suite が無く未実施（p011）。
