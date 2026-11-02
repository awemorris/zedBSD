<!-- awesome-plan project=zedbsd record=ws345-design -->

# WS345 設計: ブラウザの thread の分け方

- 2026-11-02 P8（q1959-i01、ws345-p001）。基点 main 4249e9a927。親: [ws.md](ws.md)。
- ユーザーの決定（ws.md の「ユーザーの決定」）が正。この文書はその実現の設計。設計の敵対的レビューの結果と反映は §12。

## 1. 今の作り（調べ）

### 1.1 thread と event loop

三つの platform（zedBSD の Keiland、Linux の Keiland、Windows の OmniGrammaton）の browser は同じ shell（`userland/desktop/browser/shell/shell.c`）を使う。
違うのは libkeiland の backend だけ（Wayland: `libkeiland/ui/app.c`、Win32: `libkeiland/win32/app.c`・`window.c`）。

| thread | 作る所 | 数 | 仕事 |
| --- | --- | --- | --- |
| program の main（窓の thread） | process の始め | 1 | `shell_run` の loop。下の全部 |
| `browser-net` | `net/loader.c`（`pthread_create`） | view ごとに 1 | 非 blocking の connect・TLS・送受信を poll で多重にする。pool・memory cache・時間切れ |
| `browser-resolve` | `net/loader.c` | view ごとに 1 | `getaddrinfo`（blocking）。順に 1 件ずつ |
| `browser-storage` | `storage/queue.c` | instance ごとに 1 | cookie・cache・localStorage の backend の I/O |
| libmedia の engine | `libmedia/engine.c` | 再生中の media 要素ごとに 1 | 動画の file の読みと decode |

`shell_run` の loop の 1 周（どの platform でも同じ）:

1. `kl_app_dispatch`（Wayland: compositor の fd と post の wake を `poll`。Win32: `MsgWaitForMultipleObjectsEx` の後に `PeekMessageW`・`DispatchMessageW`）。
2. 窓の入力を `browser_view_pointer_*`・`_key`・`_wheel`・`_compose` などで view に渡す。これらは DOM の event を出し、**page の JavaScript をその場で走らせる**。
3. titlebar（back・forward・reload・location）、touch の scroller、mail の sign-in code。
4. `browser_view_process`: loader の callback（届いた script の実行を含む、BUG-403 で 50 ms ずつに区切った）、timer、media、image の event、script の navigation、layout と paint。
5. 汚れていれば `shell_frame`: `browser_view_prepare`（layout と display list）と `shell_present_frame`（Vulkan の swapchain に `browser_view_record` で描いて submit・present）。

つまり **窓の thread が JavaScript・style・layout・paint・GPU の submit を全部行う**。通信は loader の thread で非同期だが、下の 1.2 の所だけは窓の thread で同期に socket を使う。

### 1.2 窓の thread を止める所

| 所 | code | 止まる長さ |
| --- | --- | --- |
| script の実行（timer・event の listener・届いた script） | `bind_run_script` ほか | script の長さ（無限 loop なら永久） |
| 同期の XMLHttpRequest | `page/script.c` の `script_fetch_sync` → `page_fetch` → `net_http_fetch`（blocking の connect、read は `NET_HTTP_TIMEOUT` 30 s） | connect は system の SYN の再送（Linux で約 127 s）、read は 30 s |
| 先取りの scan に無い parser の script（`document.write` が書いた `<script src>` など） | `script_fetch_file` → `page_fetch` | 同上 |
| iframe の文書の `<script src>` | `page/frame-load.c` の `page_fetch` | 同上 |
| parser の script の前の sheet の待ち | `page_wait_sheets`（`net_loader_wait` を 10 ms ずつ、最長 30 s） | 先取りで多くは 0。漏れた sheet は最長 30 s |
| 解析・style・layout・paint | `page_load_begin`・`geometry_layout`・`paint_build` | 重い頁で秒の単位（BUG-411: store.google.com で 1 回 0.8〜0.9 s の layout を何度も） |
| image の decode | `images_arrived` の中 | 画像の大きさに比例 |
| 名前の解決 | `http.c` の `wb_os_resolve`（同期の道だけ） | DNS の時間切れ |
| Windows の 2 つ目の起動 | `single.c` の `WM_COPYDATA`（5 s で諦める） | 窓の thread が止まっていれば URL が渡らない |

Windows では窓の thread が 5 s message を取らないと system が「応答なし」にし、title bar の移動・大きさの変更（system の modal loop は `DispatchMessageW` の中）も止まる。
zedBSD・Linux の Keiland では titlebar の control は compositor が描くが、押した結果（`KL_WINDOW_ACTION`）は窓の thread が処理するので反応しない。再描画・resize も止まる。

### 1.3 計測（host、2026-11-02 P8）

道具: [tests/stall-probe.c](tests/stall-probe.c)・[tests/stall-probe.sh](tests/stall-probe.sh)。今の shell と同じ 1 本の loop（窓の待ち → 入力 → `browser_view_process` → 汚れていれば `prepare` と `draw_pixels`）を、窓の代わりの pipe（5 ms ごとに時刻の印）で回し、印から loop が取るまでの遅れを測る。
loop が終わりの時刻を過ぎても戻らない時は watchdog の thread がその時までの遅れを書いて終える。engine は `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`（warning 0）。
相手は probe の中の 127.0.0.1 の server。「connect が掛かったまま」は accept の queue が満ちた listener（SYN が捨てられる）、「read が掛かったまま」は accept して何も返さない listener。

| 場面 | 窓の event の最長の遅れ | 100 ms を超えた event |
| --- | --- | --- |
| idle（静かな頁、床） | 55〜67 ms | 0 |
| timer の script が 3 s 回る | 2997 ms | 566 / 1173 |
| mousedown の listener が 1 s 回る | 995 ms | 175 / 1171 |
| 同期の XHR、connect が掛かったまま | 40 s を超えてまだ止まっている（40 s で打ち切り） | — |
| 同期の XHR、read が掛かったまま | 30029 ms（`NET_HTTP_TIMEOUT`） | 5841 / 7799 |
| `document.write` の `<script src>`、connect が掛かったまま | 41 s を超えてまだ止まっている | — |
| `<img>` と `fetch()`、connect が掛かったまま（loader の道） | 56.6 ms（止まらない） | 0 |
| 6000 個の box の頁を 250 ms ごとに resize | 492 ms | 678 / 1173 |
| 3000 × 3000 の PNG の decode | 1463 ms | 266 / 1170 |

- 床の 55〜67 ms は、probe の loop の待ちの上限（50 ms）と 1 回の `browser_view_process` の区切り（`VIEW_DISPATCH_NS` 50 ms）から来る。
- QEMU・実機・Windows では測っていない（未実施）。
- 読み: loader の道（非同期）は止まらない。止まるのは、窓の thread で走る script・style・layout・decode と、窓の thread に残った同期の socket（XHR・漏れた script）。

## 2. 目標と決まった事（ユーザーの決定から）

ws.md の「ユーザーの決定」の要点（D1〜D9）:

- D1 窓の event の thread は libbrowser の仕事を何もしない（XHR・fetch・script・layout をしない）。event を受けて渡すだけ。
- D2 通信は全部、専用の thread で行う。同期の XHR は呼んだ script の thread を止め、窓の thread は止めない。通信は同期の呼び出しのままでよい。
- D3 script は専用の thread で動く。iframe ごとに、上の文書と別の script の thread を持つ。
- D4 style sheet と CSS（cascade、computed style、描画の属性の更新）は専用の「描画の属性の更新」の thread で行う。
- D5 描画（paint の GPU への submit）は、できるなら shell が作る thread で、event の処理と別に行う。
- D6 thread は libbrowser が持つ（どの thread が幾つ、名前、何を走らせるかを libbrowser が決める）。三つの shell は一つの実装を共有する。
- D7 libbrowser は自分で thread を作らない。shell が登録する関数の表（thread の開始・join・lock・wait）を通して作る。
- D8 待つ処理は数を明示した名前付きの thread で同期に行う。数の無い pool にしない。場当たりで直さない（Keiland の方針、WS309）。
- D9 完了の通知は今ある wake（libkeiland の `kl_app_post`、libbrowser の `browser_wake`）と mutex・condition の形で行う。fd の event を足さない。

## 3. thread の表

### 3.1 一覧

| thread（名前） | 作る | 数 | 持つ物（唯一の書き手） | 触ってよい物 | 待つ所 | 結果の返し方 |
| --- | --- | --- | --- | --- | --- | --- |
| 窓（program の main） | shell（process の始め） | process に 1 | 窓・`kl_app`・titlebar・touch の scroller・mail・shell の状態 | view の front の口（§5.2）だけ | `kl_app_dispatch` | 入力・命令を view の front に積む（lock と condition の signal） |
| 描画（shell の `browser-render` などの名、shell が決める） | shell | 窓に 1 | swapchain・GPU の device と queue・libbrowser の renderer（pipeline・glyph の atlas・image の texture） | 確定した frame（§6）、`browser_view_render*` | shell の condition（新しい frame・resize・閉じる） | present の後に `browser_view_presented`（描画の機会を top の script の thread へ） |
| `browser-script`（top） | libbrowser（shell の表で） | view に 1 | top の browsing context: その文書の DOM・JS の heap と realm・timer・task の queue・session history・navigation・scroll の位置・入力の routing | その frame の style の結果（§4.3 の規則で）、loader の client、storage の queue | 自分の mailbox の condition（期限は次の timer） | 状態の snapshot を publish して `wake`（窓へ） |
| `browser-script`（iframe） | libbrowser | iframe（入れ子も含む）ごとに 1。1 つの view で top を含め最大 `VIEW_FRAMES_MAX` = 64（超えた iframe は browsing context を持たない、§9） | その iframe の browsing context（同じ内容） | 同上 | 同上 | 親の frame の task の queue・確定した frame |
| `browser-style` | libbrowser | view に 1 | style の engine（cascade・computed style の cache）・layout の木・display list の組み立て・layout 用の font の face | 借りた DOM（§4.2 の lend の間だけ） | 自分の request の queue の condition | request の返事（condition）、確定した frame（§6） |
| `browser-net` | libbrowser | view に 1 | socket・TLS・request の状態・接続の pool・memory cache | cookie の jar（lock）、cache の store | socket と control の waker の `poll` | 要求した thread の loader の client の mailbox に完了を積んで、その thread を起こす |
| `browser-resolve` | libbrowser | view に 1 | 名前の解決の job | — | job の queue の condition | 答えを net の thread の waker で |
| `browser-storage` | libbrowser | instance に 1 | storage の backend の I/O | — | queue の condition | 同期の読みは condition、書きは返事なし |
| `browser-media` | libbrowser（libmedia の engine を shell の表で） | 再生中の media 要素ごとに 1 | 1 つの media file の読みと decode | — | engine の condition | picture の ring（lock） |

- view は窓（tab）一つの単位。今の shell は process に窓が一つで、view も一つ。
- 一つの view の libbrowser の thread の数: script が 1 + iframe の数（最大 64）、style 1、net 1、resolve 1、それに instance の storage 1、再生中の media の数。
- `browser-resolve` を 1 本のままにする理由: 名前の解決は loader の時間切れ（30 s）と同じ道で失敗し、窓も script も止めない。遅い DNS が他の host の解決を待たせる事（head-of-line）は §11 の危険と Future Work の候補に置く。
- `browser-net` を blocking の connect の thread の pool にしない理由: 今の loader は非同期の connect・TLS の状態の機械を 1 本の名前付きの thread で動かしていて（D2 の「専用の thread」を満たす）、同時の接続の数に thread の数が要らない。D2 は同期の呼び出しを「してよい」で、強いてはいない。同期にすると、長く開いたままの接続（長い poll など）の数だけ thread が要り、数を固定すると要求が待たされる。だから今の loader の形を保ち、窓・script の thread に残った同期の socket の道を loader に移す（§7）。この判断が製品の方向に触れるなら Q1 が覆してよい（§10 の J1）。

### 3.2 shell が作る thread と libbrowser が作らせる thread

- 窓と描画の thread は shell の物。描画の thread は shell の都合（swapchain・surface・present の待ち）で作り、libbrowser の `browser_view_render*` をその thread から呼ぶ（D5）。
- libbrowser の thread（`browser-*`）は libbrowser が決めて、shell が登録した表（§5.1）で作る（D6・D7）。

## 4. 持ち主の模型

### 4.1 frame（browsing context）と agent

- 一つの frame（top か iframe）の文書・DOM・JS の realm・timer・task の queue は、その frame の `browser-script` の thread が持つ。
- **agent**: 同じ origin の frame の集まり（HTML の similar-origin window agent を origin で区切る、origin-keyed）。agent ごとに JS の heap が一つ、lock（agent の lock）が一つ。
  - 同じ agent の frame は互いの window・DOM を同期に触れる（`parent.document`、`frame.contentWindow.f()`）。仕様では同じ agent は一つの event loop を共有するので、同じ agent の script の thread は agent の lock を取って交代で走る（同時に走らない）。他の frame の関数を呼ぶと、その呼び出しは呼んだ thread で走る（仕様と同じ）。
  - 違う agent の frame は別の heap で、本当に並んで走る。互いへの access は、仕様で cross-origin に許された WindowProxy・Location の操作だけで、message で渡す（§8）。
  - `document.domain` の setter は agent を合わせない（origin-keyed の agent の集まりでは仕様が setter を効かなくする）。
- 段 A と段 B: 今の code は iframe を親と同じ heap・agent に入れている（`bind/frame.c` の先頭）。段 A は全 frame を一つの agent（一つの heap、一つの lock）のまま、iframe ごとの thread を入れる。段 B で origin ごとの agent に分け、cross-origin の iframe を別の heap で並べて走らせる（Phase は ws.md）。段 A の間は、iframe の無限 loop は同じ view の他の frame の script を止めるが、窓と描画は止めない。

### 4.2 DOM の貸し（script の thread と style の thread）

style の thread（D4）が DOM を読み、script の thread が DOM を書く。同時に触ると race になるので、DOM は **同期の貸し** でだけ style の thread に渡る:

1. script の thread（agent の lock を持ったまま）が style の thread に request（frame、種類、DOM の世代）を積み、condition で返事を待つ。待つ間、その thread は何もしない。
2. style の thread はその frame の DOM・style sheet を読み、その frame の style の結果（computed style の cache）・layout の木・display list を作る（書くのは style の物だけ）。
3. 返事（値か、描画の更新の終わり）を置いて script の thread を起こす。

request の種類:

| 種類 | 出す時 | style の thread がする事 |
| --- | --- | --- |
| 描画の更新 | event loop の「描画の更新」（§6.2）、汚れている時 | style・layout・display list を作り、frame を確定（§6） |
| style の読み | `getComputedStyle`（layout に依らない property）、`matches` の一部など、style が古い時 | 要る要素の style（cascade）まで |
| 形の読み | `offsetWidth`・`getBoundingClientRect`・`scrollHeight`・layout に依る computed style など、layout が古い時 | style と layout（BUG-411 で入れた「layout に依らない値は layout しない」はそのまま style の thread の中で効く） |
| 当たり判定 | 入力の routing（§5.3）、layout が古い時 | style と layout と hit test |

**古くない時は貸さない**: style の結果と layout の木は、貸しの間だけ style の thread が書き、それ以外の時は誰も書かない。script の thread はその frame の DOM の世代が前の更新の時と同じなら、style の thread を呼ばずに結果を直に読む（style の thread がその frame の物を書くのは、その frame の script の thread が待っている間だけなので、同時に触る事が無い）。
`offsetWidth` を DOM を変えずに何万回も読む頁（BUG-411 の store の頁）は、最初の 1 回だけ貸す。

**この形で得る物と得ない物**: style・layout は script の thread を止める（同期の読みは仕様で避けられない）。窓と描画は止めない。違う frame（段 B の違う agent）の style の更新は style の thread で順に処理される。
style と script を同じ frame で並べて走らせる事（DOM の写しの上で style を計算する）は、この WS ではしない（Future Work の候補）。

### 4.3 共有の物

| 物 | 持ち主 | 他の thread からの access |
| --- | --- | --- |
| 解いた画像の pixel | 解いた thread が作り、作った後は変えない | 参照の数（atomic）で script・style・描画の thread が共有 |
| web font の bytes | loader の完了を受けた thread | 変えない bytes を参照の数で共有。face は thread ごとに開く（style 用は style の thread、glyph の rasterize 用は描画の thread） |
| cookie の jar | — | jar の lock（今の `cookie.c` の lock を表の mutex に） |
| storage の queue | `browser-storage` | queue の lock と condition（今のまま、表の物で） |
| loader の client | 要求した thread | client の lock（§7） |
| frame の木（親子・名前・origin・閉じたか） | top の script の thread | page の lock の下で書き、他の frame は lock の下で読む（`window.length`・`frames[i]`・`parent`・`top`） |
| 確定した frame | style の thread が作り、描画の thread が読む | 確定の後は変えない。参照の数（atomic） |

## 5. libbrowser の口

### 5.1 shell が登録する thread の表（D7）

```c
/* The program's threads and their synchronization, which the engine starts and uses through (version 4). */
struct browser_threads {
	int version;
	void *context;
	int (*start)(void *context, const char *name, size_t stack_size, void (*run)(void *argument), void *argument, void **thread);
	void (*join)(void *context, void *thread);
	int (*mutex_create)(void *context, void **mutex);
	void (*mutex_destroy)(void *context, void *mutex);
	void (*mutex_lock)(void *context, void *mutex);
	void (*mutex_unlock)(void *context, void *mutex);
	int (*cond_create)(void *context, void **cond);
	void (*cond_destroy)(void *context, void *cond);
	void (*cond_wait)(void *context, void *cond, void *mutex);
	int (*cond_wait_for)(void *context, void *cond, void *mutex, uint32_t milliseconds);
	void (*cond_signal)(void *context, void *cond);
	void (*cond_broadcast)(void *context, void *cond);
};

/* Gives the engine the program's threads, once per process, before any other call; the table must outlive the process's use of the engine. */
int browser_threads_set(const struct browser_threads *threads);
```

各項目が要る理由（最小の組）:

| 項目 | 理由 |
| --- | --- |
| `start`（名前・stack の大きさ付き） | libbrowser の thread を作る唯一の道。名前は libbrowser が決め（`browser-script` など、15 bytes まで）、shell が system の名前に付ける。stack の大きさは script の thread に要る（VM の再帰と保守的な stack の走査。Windows の既定 1 MiB では足りない）。libbrowser は script の thread に 8 MiB、他に 256 KiB を頼む |
| `join` | view・instance・frame を閉じる時に、その thread が終わるのを待ってから memory を放す。detach は無い（libbrowser の thread は全部 join する。§9 の閉じ方） |
| mutex の 4 つ | 共有の物（§4.3）、mailbox、request の queue |
| condition の `wait` | mailbox が空の間の待ち、貸しの返事の待ち、同期の XHR の待ち |
| condition の `wait_for` | 次の timer までの待ち（script の thread）、loader の時間切れ。`0` か `ETIMEDOUT` を返す |
| `signal`・`broadcast` | 一人を起こす（mailbox）、皆を起こす（閉じる時、storage の同期の読みの待ち手） |

表に入れない物と理由:

- 一度だけの初期化（今の `pthread_once`）・thread ごとの値（今の `pthread_key`）: C11 の atomic で libbrowser の base が作る（`base/once.c`）。thread ごとの値は使わない形にする（今の「thread の既定の instance」は process の既定の instance にし、atomic で作る）。
- sleep: `cond_wait_for` で足りる。atomic: C11 の `<stdatomic.h>`。
- socket の waker（net の thread の `poll` を起こす）: thread でなく I/O なので libbrowser の `base/os-net-*` のまま。

契約:

- `browser_threads_set` は process で一度、libbrowser の他の全ての呼び出しの前（`browser_instance_create`・`browser_view_create`・`browser_add_ca_file`・`browser_script_tool` の前）。二度目は `EBUSY`、表の欠け・版の違いは `ENOTSUP`。表が無いまま instance・view を作ると `ENOTSUP`（`browser_script_tool` は thread を使わないので表が無くてよい）。
- 表の関数は libbrowser のどの thread からも呼ばれる。shell の実装は thread-safe であること。
- `start` が失敗（`EAGAIN`・`ENOMEM`）した時: view・instance の作成の中なら、それまでに始めた thread を止めて join し、作成が `EAGAIN` で失敗する（何も残らない）。iframe の thread なら、その iframe は browsing context を持たず（`contentWindow` は `null`）、console に error を一行、親は続ける。再試行はしない。media の thread なら、その media 要素は `error` の event（`MEDIA_ERR_DECODE` でなく `MEDIA_ERR_ABORTED`）。
- `mutex_create`・`cond_create` の失敗は `ENOMEM` として作成の失敗に。lock・unlock・wait・signal は失敗を返さない（system がそれを失敗するのは program の誤り。shell の実装は abort してよい）。
- libbrowser の thread は全部、作った view・instance・frame の destroy の中で join される。shell は自分で join・detach しない。

各 shell の実装:

- zedBSD・Linux の Keiland と Windows の OmniGrammaton: shell（`browser/shell/threads.c`、三つの platform に共通）が libkeiland の `kl_thread_start`・`kl_thread_join`・`kl_mutex_*`・`kl_cond_*` で実装する。libkeiland は zedBSD・Linux で POSIX threads、Windows で Win32 の thread（`libkeiland/platform/thread-win32.c`）なので、shell の code は一つ。`kl_thread_options` の `stack_size` と `name` をそのまま使う。
- 窓の無い program（browser の headless の mode は同じ program なので同じ表）。host の試験と `userland/tests/browser-probe` は POSIX threads の小さな実装を持つ（試験の側）。
- libbrowser の中の `pthread_*` は全部この表に替える（`net/loader.c`・`cache.c`・`cookie.c`・`tls.c`・`url.c`・`storage/queue.c`・`storage/instance.c`・`text/font.c`・`base/os-file-posix.c`）。Windows の build は libbrowser に winpthreads が要らなくなる。
- libmedia: engine の thread の作成だけを表で行う（engine を開く時に libbrowser が表の `start`・`join` を渡す）。libmedia の中の lock（`picture.c` など）は libmedia が videoplayer などとも共有する library なので pthread のまま（D7 は thread の作成の決定で、libmedia の lock は libbrowser の lock ではない）。覆す時は Q1（§10 の J3）。

### 5.2 view の口（threaded の mode）

`struct browser_view_options` に `threading`（`BROWSER_THREADING_CALLER`: 今のまま呼んだ thread で全部、headless・試験・tool 用 ／ `BROWSER_THREADING_OWN`: libbrowser の thread で、窓の program 用）と、描画の thread への wake（`frame_wake`、`frame_wake_context`）を足す（`BROWSER_API_VERSION` 4）。

`BROWSER_THREADING_OWN` の時、呼んでよい thread:

| 口 | thread | する事 |
| --- | --- | --- |
| `browser_view_create`・`_destroy` | 窓 | top の script・style・net・resolve の thread を始める ／ 止めて join（§9） |
| `_load`・`_follow`・`_go`・`_stop`・`_resize`・`_set_color_scheme`・`_scroll_to`・`_set_overscroll` | 窓 | 命令を mailbox に積む（返すのは `0`・`ENOMEM`・`ENOBUFS`） |
| `_pointer_*`・`_wheel`・`_key`・`_focus`・`_compose`・`_commit_text`・`_focus_field` | 窓 | 入力を mailbox に積む（§5.3 の順と合わせ方） |
| `_url`・`_title`・`_can_go`・`_load_progress`・`_scroll_y`・`_document_height`・`_scroll_range`・`_text_session`・`_text_target`・`_text_context` | 窓 | 最後に publish された snapshot を返す。文字列は次の `browser_view_process` まで有効 |
| `browser_view_process` | 窓 | snapshot を入れ替え、積まれた通知（redraw・title・committed・load・console・script_error・image）の callback を窓の thread で呼ぶ。engine の仕事はしない（D1） |
| `browser_view_timeout` | 窓 | `-1`（timer は script の thread が持つ） |
| `browser_view_request_image`（新、drag の画像） | 窓 | 要求を積む。答えは callback `image`（新）で窓の thread に |
| `browser_view_set_gpu`・`_render`（新）・`_render_pixels`（新）・`_presented`（新）・`_release_targets`・`_gpu_failure` | 描画 | §6 |
| `_settle`・`_dump`・`_draw_pixels`・`_prepare`・`_draw`・`_record`・`_image_at` | — | `BROWSER_THREADING_OWN` では `ENOTSUP`（headless の mode 用） |

- callback `link`（link を辿るかの判断）は script の thread で呼ばれる（答えがその場で要るため）。shell の実装は印字して ALLOW を返すだけで、窓の状態に触れない。契約に「script の thread で呼ばれる。view と窓に触れない」と書く。
- 窓の thread が libbrowser でする事は、mailbox への copy と snapshot の読み（文字列の copy）だけで、DOM・JS・style・layout・通信に触れない（D1）。

### 5.3 入力の渡し方と順

- mailbox は view に一つの FIFO（lock と condition）。窓の thread が積み、top の script の thread が取る。
- 合わせる物: 続けて来た pointer の移動は最後の一つに（間に離散の入力が無い時）、続けて来た wheel は差を足す、resize と color scheme は最後の一つ。離散の入力（button・key・text・focus）は合わせず、捨てず、順を保つ。
- 上限: 離散の入力は 1024 件。超えると積む口が `ENOBUFS` を返し、shell はその入力を捨てる（長い script の間に 1024 回押した場合だけ）。
- 順: 一つの FIFO なので、窓が受けた順に page が受ける（移動と押しの間の順を含む）。命令（navigation など）も同じ FIFO で、入力との前後を保つ。例外は §9 の「打ち切り」だけ（積むと同時に flag を立てる）。
- routing: top の script の thread が当たり判定（§4.2、古くなければ直に）をし、iframe に当たれば座標を直して、その iframe の script の thread の task の queue に積む（段 B では別の agent の queue）。
- 長い script の間の入力は FIFO に残り、script が終わった後に順に処理される（どの browser も同じ。窓・描画・titlebar は止まらない）。

### 5.4 待ちと起こし方（D9）

| 待つ thread | 待つ物 | 起こす者 |
| --- | --- | --- |
| 窓 | `kl_app_dispatch`（今のまま） | libbrowser が `wake` を呼び、shell が `kl_app_post`（今の `shell_wake` のまま） |
| 描画 | shell の condition | libbrowser が `frame_wake` を呼ぶ（新しい frame の確定）、窓の thread が resize・閉じる |
| script | mailbox の condition（`cond_wait_for`、期限は次の timer） | 窓の命令と入力、loader の client の完了、style の返事、親子の frame の task、`browser_view_presented` |
| style | request の queue の condition | script の thread の request |
| net | socket と control の waker の `poll`（今のまま） | 要求・取り消し（waker を鳴らす） |
| resolve・storage・media | 各 queue の condition（今のまま、表の物で） | — |

## 6. 描画（D5）

### 6.1 確定した frame

- 描画の更新（§6.2）の終わりに、style の thread がその browsing context の **確定した frame** を作る: display list、その item が指す layout の木（今の `paint_frame` と同じく木ごと持つ）、画像の pixel と font の bytes への参照、大きさ、scroll、canvas の色、通し番号。作った後は誰も書かない。参照の数は atomic。
- iframe は親の list の中で「frame の item」（新しい種類、frame の id と枠と clip）になり、描画の thread が描く時にその id の最新の確定した frame を引く。iframe は親と別に確定できる（段 B の別の agent でも）。
- 確定は view の「frame の箱」（lock の下で frame の id ごとの最新の参照）に置き、`frame_wake` を呼ぶ。古い frame は描画の thread が放した時に消える。
- 確定の後に木を書き換えてはいけない。今 `page_sticky_sync` は scroll の時に box を書き換えるので、scroll の変化は新しい確定（sticky の位置を display list に書いた物）にする（§11 の R5）。

### 6.2 描画の機会と rAF

- 描画の thread は present の後に `browser_view_presented(view, serial)` を呼ぶ。libbrowser は top の script の thread（と各 iframe の thread）に「描画の機会」を積む。
- script の thread は task の後に描画の機会があり汚れていれば、HTML の「描画の更新」をする: resize・scroll の event、`requestAnimationFrame` の callback、そして style の thread への描画の更新の request（§4.2）。
- 確定は present の一つ先までにする（描画の thread がまだ使っていない確定が一つあれば、次は機会を待つ）。描画の thread が止まれば rAF も止まる（隠れた窓と同じ）。
- 今の `requestAnimationFrame` は「一 frame 先の timer」（`bind/timer.c`）。これを描画の機会に結ぶ。

### 6.3 描画の thread の 1 周（shell の物）

```
for (;;) {
	wait (shell's condition) for a new frame (frame_wake), a resize, or the end;
	acquire a swapchain image;
	browser_view_render(view, &target, commands);   /* the engine records the latest committed frames */
	submit; present;
	browser_view_presented(view, serial);
}
```

- GPU の device・queue・swapchain は描画の thread だけが使う（queue を thread で分け合わないので、queue の lock は要らない）。libbrowser の renderer（`paint/vulkan.c`）とその pipeline・glyph の atlas・image の texture も描画の thread の物。
- surface は libkeiland の `kl_window_*` で作るので窓の thread で作り、描画の thread に渡す（libkeiland の規則: 窓の呼び出しは application の thread）。present（`vkQueuePresentKHR`）は Vulkan の規則では swapchain と queue の外部の同期だけで、どの thread からでもよい。
- resize: 窓の thread が新しい大きさを描画の thread と view に渡す。描画の thread は swapchain を作り直し、新しい大きさの確定が来るまで最後の確定を新しい大きさで描く（はみ出しは canvas の色）。Windows の system の modal loop（大きさの変更・移動の間、窓の thread は `DispatchMessageW` の中）の間も描画の thread は描き続ける。
- CPU の道（`KL_PRESENT_SHM`、`--no-gpu`）: 描画の thread が `browser_view_render_pixels` で shell の pixel に描き、窓の thread に渡して、窓の thread が libkeiland の shm の present を呼ぶ（libkeiland の呼び出しは窓の thread の規則。pixel の copy は libbrowser の仕事ではない）。
- 描画の thread の text: glyph の rasterize は描画の thread が自分の face で行う（style の thread の face と共有しない）。

## 7. 通信（D2）

- 全ての http・https は loader（`browser-net`・`browser-resolve`）を通る。`BROWSER_THREADING_OWN` の view では `http.c` の同期の道（`net_http_fetch`）を使わない（`BROWSER_FETCH_AT_ONCE` の headless だけに残す）。
- loader の **client**: loader を使う thread（各 frame の script の thread、style の thread）ごとに一つ。request はその client に属し、完了は client の list に積まれ、client の notify がその thread の mailbox を起こす。callback は要求した thread で、`net_loader_dispatch(client)` から走る。取り消しはどの thread からでもできる（client の lock、net の thread の control の waker）。
- 今 page の code が窓の thread で同期に取っていた物を loader に移す:
  - 同期の XHR（`script_fetch_sync`）: loader の request を作り、script の thread が client の condition で完了か取り消しを待つ（仕様のとおり script を止める。窓は止めない）。
  - 先取りに無い parser の `<script src>`（`script_fetch_file`）・iframe の `<script src>`（`frame-load.c`）: 同じく loader で待つ（parser は止まる、仕様のとおり）。
  - `page_wait_sheets`: 今の 10 ms ずつの待ちを client の condition の待ちに。
  - fonts・sheets・images・media の「loader が無い時は今すぐ読む」の道は、`file:`・`data:` だけに残る。
- connect・TLS の handshake・read・write は `browser-net` の中で、今の非同期の状態の機械のまま（§3.1 の理由）。DNS は `browser-resolve`。

## 8. frame の間の access（D3）

- 同じ agent: 今のまま同期に直に（同じ heap）。呼ばれた側の frame の thread は agent の lock を待っているので走っていない。
- 違う agent（段 B）: 相手の window は「遠くの WindowProxy」（呼んだ側の heap の object）で、仕様で cross-origin に許された物だけを持つ:
  - `postMessage`: 呼んだ側で structured clone を bytes に書き（今の `bind/cloning.c` を heap の外の形に）、相手の frame の task の queue に積む。相手の thread がその heap で読み直して `message` を出す。`ArrayBuffer` の transfer は copy と detach。`MessagePort` の agent を越える transfer は段 B ではしない（`DataCloneError`、Future Work）。
  - `location` の代入・`replace`: 相手の frame の thread への navigation の命令。
  - `window.length`・`frames[i]`・`parent`・`top`・`opener`・`closed`: page の lock の下の frame の木から読む。
  - `focus`・`blur`: top の script の thread への命令。
- 同じ origin の frame は必ず同じ agent に入るので、同期の DOM の access が agent を越える事は無い。
- 親の DOM の iframe 要素（大きさ・`src` の変更）は親の thread の物。`src` の変更は子の thread への navigation の命令。iframe の要素が外れると、子の browsing context を閉じる（§9 の打ち切りと join、子の子も）。

## 9. 取り消し・打ち切り・閉じ方

- **打ち切り（termination）**: agent ごとの atomic の flag。VM は loop の印（`VM_OP_LOOP_HINT`）・関数の入口・長い native（sort・正規表現の match・JSON など）で見て、立っていれば catch も finally も走らない終わり（今の `VM_THROWN` と別の status）で task を抜ける。microtask も捨てる。
- 立てる時: 窓の thread が navigation（`_load`・`_follow`・`_go`・reload）か `_destroy` を積んだ時に、top の agent の script が task の中なら、積むと同時に立てる（古い頁は消えるので、その頁の script を待つ理由が無い）。iframe を外す・iframe の navigation は、その iframe の agent の、その frame の task に限って立てる（段 A では一つの agent なので、その frame の thread が走っている時だけ）。
- `_stop` は読み込みを止める（loader の request を取り消す）が script は打ち切らない（どの browser も同じ）。止まらない頁の扱い（「応答しない頁」の問い）は製品の判断（§10 の J2）。
- 通信の取り消し: navigation・閉じる時は、その frame の loader の client の request を全部取り消す。net の thread は非同期の connect・TLS の socket をすぐ閉じる。resolve の thread の中の `getaddrinfo` は止められず、答えは番号で捨てられる。同期の XHR の待ちは取り消しで起こされ、打ち切りの flag で script も抜ける。
- `browser_view_destroy`（窓の thread）: 打ち切りを立てる → 全 frame の script の thread に「終わり」を積む → join（子から）→ style・net・resolve を止めて join → 描画の thread には shell が先に `browser_view_render` を呼ばなくした事を約束させる（destroy の前に shell が描画の thread を止める）。resolve の join は `getaddrinfo` の時間切れまで待つ事がある（§11 の R7）。shell は窓を先に閉じてから destroy する。

## 10. 判断の点（Q1 へ）

- J1（技術、P8 の判断。製品の方向なら Q1）: 通信は今の非同期の loader（名前付き 1 本）を保ち、同期の connect の thread の pool にしない（§3.1）。
- J2（製品）: 長く走る script のある頁に「応答しない頁（待つ／頁を閉じる）」の問いを出すか。出さない場合も、navigation と閉じるは打ち切りで効く。今の案は出さない（この WS の範囲の外）。
- J3（技術、P8 の判断）: libmedia の中の lock は pthread のまま。thread の作成だけ表を通す（§5.1）。
- J4（技術、P8 の判断）: 1 つの view の frame の上限 64（top を含む）。`FRAME_MAX_DEPTH`（深さ 64）と別に、数の上限を置く。

## 11. 危険

| # | 危険 | 対処 |
| --- | --- | --- |
| R1 | data race（thread を分けると今まで一つの thread の前提だった static な変数が壊れる。`bind/computed.c` の `computed_pseudo`・`computed_zoom`、`page/svg.c` の `svg_view_*`、`css/cascade.c` の `cascade_animation_suppressed`、`layout/position.c` の `position_remember`、`net/http.c` の `http_fetch_kind`） | 実装の Phase の最初に、process に一つの可変の static を全部洗い、thread（style か script）の context に移す。host の試験を ThreadSanitizer でも流す（`-fsanitize=thread` の variant を probe の script に） |
| R2 | 再入（style の request の中から script を呼ぶ道。今の layout の中の `ResizeObserver`・container query・画像の到着の callback） | style の thread は JS を呼ばない。今 layout・style の中から JS を呼ぶ所は、style の thread が「後で出す event」の印を返し、script の thread が返事の後に出す |
| R3 | 同期の API（`alert`・`confirm`・`prompt` は今無い。同期の XHR） | 同期の XHR は §7。将来の dialog は script の thread が窓の答えを condition で待つ形（窓の thread は止めない） |
| R4 | JS からの layout の読み（`offsetWidth` を何万回） | 古くない時は貸さない（§4.2）。貸しの往復は 1 回 数十 µs の見込み。試験で数える |
| R5 | 確定した frame の木の書き換え（`page_sticky_sync`、画像の到着で box の画像を差し替える所など） | 確定の後は書かない。scroll・sticky は新しい確定に。debug の build で確定した木の checksum を描画の後に比べる |
| R6 | GC の stack の走査（今 heap は一つの thread の stack を走査する、`vm_heap_set_stack_base`） | agent の lock を取った thread が自分の stack の底を heap に置き、lock を返す時に外す。lock を待つ thread は task の境でだけ待つ（VM の値を stack に持たない）。同期の XHR・貸しの待ちは lock を持ったまま（同じ agent の他の frame は走らない） |
| R7 | 閉じる時の resolve の join が DNS の時間切れまで待つ | 窓を先に閉じる。process の終わりなので利用者には見えない。Future Work: 中断できる解決（Windows の `GetAddrInfoExW` の cancel など） |
| R8 | Wayland の configure の ack（窓の thread）と描画の thread の commit の大きさの食い違い | 描画の thread は ack された大きさで swapchain を作り直してから present する（窓の thread が ack した大きさを渡す）。Keiland の compositor で resize の試験 |
| R9 | font の face の共有（layout と glyph の rasterize） | thread ごとに face を開く（§6.3）。memory は bytes を共有して face の構造だけ増える |
| R10 | iframe の多い頁の thread の数（広告の iframe） | 上限 64（J4）。stack は予約だけで、触った分だけ memory になる |
| R11 | 段 B の structured clone の対応の漏れ（heap を越える形） | 段 B の Phase で WPT の `postMessage` の試験の部分集合 |
| R12 | `BROWSER_THREADING_CALLER` と `_OWN` の二つの道の分かれ | 二つの道は front（積むか、直に呼ぶか）だけが違い、page の code は同じ。host の試験を両方の mode で流す |

## 12. 設計の敵対的レビュー

（design-reviewer の結果と反映をここに書く。）
