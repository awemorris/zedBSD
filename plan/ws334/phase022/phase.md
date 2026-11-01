<!-- awesome-plan project=zedbsd record=ws334-p022 -->

# ws334-p022 統合: network を OS の層へ（loader・http・tls）と libmedia の起こしの callback

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.2 の O1〜O12・O25・O32・O33、§3.4、§5 の M1、§9.1。
- 状態: cleared（2026-11-01 09:45 Q1 の判定、main 20f6a10fc。P7 実装済み・試験 PASS: run-unit all 34 本・cross-build・fetch-wine・host の loader・http・fetch）
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: 名前の表に Windows の DLL の名前（`libssl-3-x64.dll`・`libcrypto-3-x64.dll` ほか）を足さない。Windows の build は p030 の前は OpenSSL を開けずに今の EPROTONOSUPPORT、p030 で `net/tls-static.c` が表を埋める。下の本文の DLL の名前はこの通りに読み替える。
- 依存: p007（os-net）・p021。
- 担当の想定: Opus（phase-runner）を 1 回。module の約束は `base/os-net.h`・`base/os-system.h` の comment。挙動を変えない（zedBSD・Linux の host の network の試験の件数が前と同じ）。

## 調整（始める前）

- `net/loader.c`（2,482 行、P8 と重なりが中〜高）・`net/net.h`・`page/media.c` を触ることを Q1 に送り、P8 の作業と当たらない時に始める。

## 変える file と中身

### net/loader.c

1. include: `<fcntl.h>`・`<netdb.h>`・`<poll.h>`・`<sys/socket.h>`・`<unistd.h>`・`<pthread_np.h>` と thread の名前の `#if defined(__linux__) || defined(__FreeBSD__)` を外し、`base/os-net.h`・`base/os-system.h`・`base/os-time.h` を include。`<pthread.h>` は残す。
2. 起こし: `pipe_read`・`pipe_write`（resolver の答え）と `control_read`・`control_write`（制御）の 2 組の pipe を `struct wb_os_waker *answers` と `struct wb_os_waker *control` にする。1 byte の `write` は `wb_os_waker_ring`、読み切りは `wb_os_waker_drain`、`close` は `wb_os_waker_close`。
3. 待ち: `struct pollfd *fds`・`fds_capacity` を `struct wb_os_poll_item *items`・`items_capacity` に。`POLLIN` → `WB_OS_POLL_READABLE`、`POLLOUT` → `WB_OS_POLL_WRITABLE`、`revents` の `POLLERR|POLLHUP|POLLNVAL` → `found & WB_OS_POLL_FAILED`。`poll(fds, count, timeout)` → `wb_os_poll(items, count, timeout, &ready)`（誤りの値の扱いは今の poll の失敗と同じ）。capacity の計算から `LOADER_WATCH_MAX` を除く。
4. socket: `request->descriptor`（int、-1）を `wb_os_socket socket`（`WB_OS_SOCKET_NONE`）に。`loader_connect` の `socket`・`fcntl(O_NONBLOCK)`・`connect` を `wb_os_socket_connect(request->addresses, request->address_index, 0, &request->socket)`（0 と EINPROGRESS を今の「続く」に）、`getsockopt(SO_ERROR)` を `wb_os_socket_connect_result`、`send(..., MSG_NOSIGNAL)` を `wb_os_socket_send`（EAGAIN は今の EAGAIN・EWOULDBLOCK と同じ扱い）、`recv` を `wb_os_socket_receive`、`close` を `wb_os_socket_close`。pool の kept の connection（:2293）も同じ。
5. 名前の解決: `struct addrinfo *addresses`・`*address`（次に試す物）を `struct wb_os_addresses *addresses` と `size_t address_index`・`size_t address_count` に。resolver の thread の `getaddrinfo`（hints は os-net.h が同じ物を使う）を `wb_os_resolve(job->host, job->port, &job->addresses)`、`freeaddrinfo` を `wb_os_addresses_free`。失敗の時の `EHOSTUNREACH` は今のまま。
6. thread の名前: `loader_name_thread` の中身を `wb_os_thread_name`。
7. 時計: `loader_clock` の `clock_gettime(CLOCK_MONOTONIC)` を `wb_os_monotonic_ns` に。`net_loader_wait` と `changed` の cond（:381 の `pthread_condattr_setclock(CLOCK_MONOTONIC)`、:661〜:690 の絶対の時刻の待ち）は、setclock を使わない形に替える（llvm-mingw の winpthreads の `pthread_condattr_t` は int で時計を持てず、MONOTONIC の期限が過ぎた時刻に見えて待ちが回り続ける見込み。design-reviewer の指摘、2026-11-01 に決めた）: cond は属性なしで作り、待ちの loop で毎回 `wb_os_monotonic_ns` で残りの時間を求め、残りが 0 以下なら終わり、そうでなければ `clock_gettime(CLOCK_REALTIME)`（ISO C の `timespec_get(TIME_UTC)` を使う。`<time.h>` だけで両方の system にある）に残りを足した絶対の時刻で `pthread_cond_timedwait`、起きたら条件と残りを見直す。壁の時計が動いても待ちは長くも短くもならない（残りを単調の時計で求め直すため）。`<time.h>` の `clock_gettime` はこの file から無くなる。
8. watch: `net_loader_watch`・`net_loader_unwatch`・`watches`・`watch_count`・`watch_generation`・`LOADER_WATCH_MAX` と poll の中の watch の項を消し、`net_loader_ring(struct net_loader *loader)` を足す: lock の中で今の「watch が鳴った」時と同じ印（`loader->rang` を立てる、`loader.c:1122-1127` の結果と同じ）を立て、`wb_os_waker_ring(loader->control)`。どの thread から呼んでもよく、loader の lock だけを取る。
8b. poll の item の数: `items_capacity` の伸びを `WB_OS_POLL_MAX`（4096）で止め、それを超える接続は今の LOADER_PER_HOST・全体の上限の判定の前に断る（今の上限で 4096 に届かないことを確かめ、届かなければ確かめの結果だけを書く）。
9. `net/net.h`: `net_loader_watch`・`_unwatch` の宣言を消し `net_loader_ring` を足す（comment: 「media の engine の thread からも呼ばれる。印を立てて network の thread を起こすだけ」）。`net_tls_wants` の `short *wants`（:184-186）を `unsigned *wants`（`WB_OS_POLL_*`）に。socket を受ける net の関数の int の descriptor を `wb_os_socket` に（`net_tls_open` ほか。`base/os-net.h` を include）。

### net/http.c

- `http_connect`（:646-）: `getaddrinfo` → `wb_os_resolve`、各 address に `wb_os_socket_connect(addresses, index, 1, &socket)`（blocking: 今の blocking の接続と同じ）、失敗で次の address、最後の誤りを返す。`struct http_connection` の `descriptor` を `wb_os_socket`。`send(MSG_NOSIGNAL)`・`recv`・`poll`・`close` を os-net の関数に（今の poll の待ちの timeout はそのまま `wb_os_poll` に）。`<strings.h>` の `strncasecmp` などは base の大小の比べ（`wb_ascii_*` が base.h にあればそれ、無ければ http.c の static の関数）に。`http_memmem` を `wb_memmem`（p021）に替えて消す。
- `getenv("BROWSER_NET_DUMP")` などは今のまま（Windows の getenv で動く）。

### net/tls.c

- libcrypto を今は `RTLD_GLOBAL` で開いている（:653 付近）が、`wb_os_library_open` は `RTLD_LOCAL`。libssl は libcrypto を DT_NEEDED で読むので足りる見込み。zedBSD の TLS の試験（WS074 の https の試験）で確かめ、足りなければ止めて Q1 に。
- `dlopen`・`dlsym`・`dlerror` を `wb_os_library_open`・`wb_os_library_symbol`（`wb_os_function` から各関数の型へ `memcpy` か cast で。今の `tls_find` の形を保つ）・`wb_os_library_close`。名前の表を `wb_os_library_open` の配列に: libssl は {"libssl.so", "libssl.so.3", "libssl-3-x64.dll", "libssl-3-arm64.dll", NULL}、libcrypto も同じ形（system に無い名前は開けないだけなので 1 つの表でよい。`#ifdef` を書かない）。失敗の理由の文は「cannot load the OpenSSL libraries」に 1 本化（dlerror の文は OS の層に無い）。
- `SSL_set_fd(ssl, int)`: `wb_os_socket_descriptor` で int を得て渡す（ERANGE は EPROTO にして理由を残す）。
- `SSL_ERROR_SYSCALL` の後の `errno`（:302・:482）を `wb_os_socket_last_error()` に。
- `POLLIN`・`POLLOUT` を `WB_OS_POLL_READABLE`・`WRITABLE` に。`<poll.h>`・`<dlfcn.h>` を外す。
- 根の証明書（`SSL_CTX_set_default_verify_paths`）と Windows の store は p030。この Phase は変えない。

### libmedia と page/media.c（M1）

- `libmedia/media.h`: `media_engine_open(path, source, flags, engine)` を `media_engine_open(path, source, flags, wake, wake_context, engine)` にする。`wake` は `void (*)(void *context)`、NULL を許す（起こさない）。comment の「The engine never calls back」の段落を「engine は状態が変わった時・絵が来た時に engine の thread から wake を呼ぶ。呼び手は短く返し、engine の関数を wake の中から呼ばない。次に media_engine_status を読むまで重ねて呼ばれうる」に書き直す。`media_engine_wake_fd` を消す。
- `libmedia/engine.c`: wake の pipe（`made->wake`、:206-219 の `pipe`・`fcntl`、:259 の read、:1018 の write、:492-494）を消し、`made->wake_function`・`made->wake_context` を open の引数から持ち、今 pipe に書く所で `wake_function(wake_context)` を呼ぶ（NULL なら何もしない）。今 pipe を読み切る所（`media_engine_status` の中）は何もしない。
- `libmedia/exports.map`: `media_engine_wake_fd` を外す。
- `page/media.c:640`・`:670`: open に `page_media_wake`（新しい static の関数: `net_loader_ring(media->page->loader)`）と media を渡す。`:132` の unwatch と `:698-699` の watch を消す。close の順（media の close が loader の destroy の前）は今のまま。
- 他の利用者: `grep -rn 'media_engine_open\|media_engine_wake_fd' userland plan tests` で探し、全部を新しい形に（p002 の時点では libbrowser の page/media.c だけ。増えていれば Q1 に）。

## Windows の build

- `libbrowser/sources.mk` の `LIBBROWSER_OS_SOURCES` に `$(LIBBROWSER_DIR)/base/os-net-posix.c` を足す（zedBSD・Linux の host の build）。
- `apps/browser-win32/Makefile` の `libbrowser-core` から net の 3 つの除外を外し、os-net-win32.c を足す（`net/cookie.c` と `page/storage.c` は p023 まで除いたまま）。libmedia の `engine.c` は p026 で。
- `cross-build.sh` の `nm -u` の許す表に ws2_32 の関数を足す。

## 試験（受け入れ）

- 段 1: 基点（p021 の後の main）と後で同じ件数: `plan/ws074/tests/run-loader-tests.py`・`run-http-tests.py`・`run-fetch-tests.py`（無ければ WS074 の network の試験の名前を `ls plan/ws074/tests/` で探し、Q1 に確かめる）、`host-build.sh plain`・`asan`、`plan/tools/browser-component/run.sh plain asan`、libmedia の host の試験（`plan/tools/h264sw/run-unit.sh all` は libmedia の別の部分なので件数だけ確かめる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → PASS。`cross-build.sh` → PASS（net を含む libbrowser-core が両方の arch で warning 0）。
- zedBSD amd64 の `libbrowser.so`・`libmedia.so`・`bin/browser`（p020 の command）→ warning 0。
- Wine: p021 の `script-tool.exe` に loader を含めた小さな fetch の program（`plan/ws334/tests/fetch-main.c`: `net_http_fetch` で host の `python3 -m http.server` の 127.0.0.1 の port から 1 頁を取り、bytes の数を出す）が Wine で動く。https は p030。
- `git diff --check`。

## 変えてはいけない物

- module の header と .c、試験の道具。header の不足は止めて Q1 に。
- cookie と storage（p023）、TLS の根（p030）、libmedia の他の file（p026）。HAL・toolchain・共有の build/。rm をしない。push しない。

## 終わりの条件

- 上が全部 PASS（件数の前後の表を結果の節に）。file の群ごとの commit。SHA・command と最後の行・未実施を Q1 に。ws.md の行を「実装済み・試験 PASS」に。

## 結果（2026-11-01、P7、q1830-i01、基点 7b32ed291）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 3f8f2f205 | net/loader.c・net.h・http.c・tls.c、page/media.c、libbrowser/sources.mk（`LIBBROWSER_OS_NET_SOURCES`）、libmedia/media.h・engine.c |
| 1cda6e03f | apps/browser-win32/Makefile（net の 3 つを libbrowser-core に）、plan/ws334/tests の cross-build.sh・allowed-imports.txt と新しい試験（net-ring-test・media-wake-test・fetch-main.c と fetch-wine.sh） |
| 6602db9bc | net/http.c の 1 か所（条件の中の関数呼び出しを外に。動作は同じ） |

前後の件数（基点は `git archive 7b32ed291` を `build/tmp/p022-base` に展開して流した。後は worktree の tree）:

| 確かめ | 前（7b32ed291） | 後 |
| --- | --- | --- |
| `host-build.sh plain` / `asan` | exit 0、warning 0 | 同じ |
| `run-loader-tests.py`（plain / asan） | 11/11 / 11/11 | 11/11 / 11/11（6602db9bc の後も plain 11/11） |
| `run-http-tests.py`（plain / asan） | 15/15 / 15/15 | 15/15 / 15/15（6602db9bc の後も plain 15/15） |
| `run-http-tests.py --async`（plain / asan） | 19/20 / 19/20（FAIL は async-sheets） | 19/20 / 19/20（同じ async-sheets、case ごとに同じ） |
| `run-fetch-tests.py`（plain / asan） | blocking 38/38・--async 38/38 | 同じ |
| `plan/tools/browser-component/run.sh plain` / `asan` | 83 checks PASS / 83 checks PASS | 同じ |
| `plan/tools/h264sw/run-unit.sh all` | — | run-unit: PASS（24 本、tools.md の数と同じ） |

他の確かめ:

- 新しい試験: `sh plan/ws334/tests/net-ring-test.sh plain`・`tsan` → `net-ring-test: PASS`（WS314 の loader-test を写して watch の項を `net_loader_ring` に替えた: 別の thread からの ring で notify が 1 回、dispatch まで重ねて鳴らしても 1 回、4 thread × 200 回の ring と 24 本の request が同時でも全部 200 で終わる、`net_loader_wait(300)` が 290〜3000 ms、`wait(0)` が待たない）。`sh plan/ws334/tests/media-wake-test.sh plain`・`asan` → `media-wake-test: PASS`（sample.mp4 で wake が engine の thread から context 付きで呼ばれる、play の中で呼び手の thread から、close の後は呼ばれない、wake NULL でも開いて閉じる）。
- `LC_ALL=C.UTF-8 timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`（34 本: posix 16・win32 18、FAIL 0。test-os-net posix 1278・win32 1276 checks）。log `build/tmp/p022/run-unit.log`。
- `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（libbrowser-core に net/loader・http・tls が入り、x86_64・aarch64 とも warning 0。libbrowser-core の import は win64 164・winarm64 160 で全部が表の中。表に ws2_32 の 20 の関数、kernel32 の InitOnceExecuteOnce・SetHandleInformation、winpthreads の lock・cond・thread・key、UCRT の `_timespec64_get` を足した。3b の照合に os-net-win32.o を加え、除く名前を `net_` から `net_cookie_` に狭めた）。
- Wine: `sh plan/ws334/tests/fetch-wine.sh` → `fetch-wine: PASS`（test-program の `fetch-tool.exe` = fetch-main.c と net/cookie.c を全部の static library と link。host の `python3 -m http.server` の 127.0.0.1:18334 の 104535 byte の頁を、blocking の `net_http_fetch` と loader の `net_loader_fetch` の両方で 200・104535 byte。Wine の起動は script の中で `flock /tmp/zedbsd-wine.lock`）。
- zedBSD amd64（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=<写し> -o <写し>/.zedbsd-sysroot-complete -j16 build/amd64/dynamic/libbrowser.so build/amd64/dynamic/libmedia.so build/amd64/bin/browser`、写しは main の `build/amd64/sysroot` を `build/tmp/p022/sysroot-amd64` に写して tree の `include/libc`・`userland/desktop/include` を重ねた物）→ exit 0、warning 0。build の中の `python3 tools/build/check-dynamic-elf.py` は PATH の python3 で走ったので、3 つとも `/usr/bin/python3` で流し直して exit 0。libbrowser.so は `-z defs` で link（`timespec_get`・`getaddrinfo`・`poll` は libc から）。
- `git diff --check 7b32ed291` → 出力なし。

自分で決めた点・phase.md と code の食い違い:

- 手順 2: resolver の起こしの field は `answers` が既に答えの list（`struct loader_job *answers`）の名前なので `answers_waker` にした。制御は `control`。
- 手順 8: `loader->rang` は network の thread だけが触る field なので、`net_loader_ring` は lock の中で新しい `loader->rung` を立て、network の thread が `loader_intake`（lock の中）でそれを `rang` に写す（rang を他の thread から書くと data race になるため）。結果は phase.md の「watch が鳴った時と同じ」（次の round の `loader_publish` で notify 1 回・wait が起きる）。TSan の net-ring-test で race 無し。
- 手順 8b: 今の上限（LOADER_PER_HOST は場所ごとの 6 だけ、全体の上限は無い）では 4096 に届きうるので、`loader_begin` の最初（cache の答えの後、kept の接続と場所ごとの判定の前）で、自分以外の動いている request（READY・WAITING・ENDED・cancel の外）が `WB_OS_POLL_MAX - 2`（4094）以上なら EMFILE で終える（新しい `loader_connections`）。items の capacity も `WB_OS_POLL_MAX` で止める。4094 本の試験はしていない（host の fd の上限 1024 の方が先に効く）。
- 手順 7: `net_loader_wait` は phase.md の通り属性なしの cond、毎回 `wb_os_monotonic_ns` で残りを求め、`timespec_get(TIME_UTC)` + 残りで `pthread_cond_timedwait`。timeout 0 は待たない（前と同じ）。
- 手順 9 の sources.mk: phase.md は `LIBBROWSER_OS_SOURCES` に足すと書くが、p021 の記録（script の core は network を使わない）に従い、新しい `LIBBROWSER_OS_NET_SOURCES`（os-net-posix.c の 1 行）を `LIBBROWSER_ENGINE_SOURCES` の頭に入れた。`apps/browser-win32/Makefile` は `LIBBROWSER_OS_SOURCES` と `LIBBROWSER_OS_NET_SOURCES` を除き、os-net-win32.c は前から layer にある。cookie.c と page/storage.c は除いたまま（p023）。`list-sources.sh` は 1 行に 1 file なので拾う（host-build.sh は変更なし）。
- tls.c: 名前の表は p036 の読み替えの通り `libcrypto.so`・`libcrypto.so.3`、`libssl.so`・`libssl.so.3` だけ（Windows の DLL の名前は足さない）。失敗の理由は 2 回目以降も含めて「cannot load the OpenSSL libraries」（前は 1 回目が dlerror の文、2 回目以降が「the OpenSSL package is not installed」。この文を見る試験は無い）。読めた library は失敗しても閉じない（前と同じ。libcrypto が exit の後片付けを登録している場合があるため）。`SSL_set_fd` の前の `wb_os_socket_descriptor` の失敗は「setup: the socket does not fit OpenSSL's descriptor」を残して EPROTO（新しい static の `tls_attach`）。RTLD_LOCAL: host（Debian の libssl.so.3）の https の試験（run-http-tests の https の case、run-loader-tests の https-reuse）が PASS。zedBSD の OpenSSL の package の libssl.so は DT_NEEDED に libcrypto.so を持つ（readelf で確かめた）が、zedBSD の上の https は未実施。
- http.c: `strncasecmp` は static の `http_case_differs`（ASCII だけ大小を同じに、NUL で止まる）に、`http_memmem` は `wb_memmem` に（呼ぶのは needle 4 byte だけなので約束の差は出ない）。`http_receive_all` の待ちは、前は poll の EINTR で待ち直したが `wb_os_poll` は signal で ready 0 を返すので、単調の時計で NET_HTTP_TIMEOUT に届いていなければ待ち直す。
- libmedia: `media_wake_function` の typedef を media.h に足した。wake は engine の thread に加えて `media_engine_play`・`media_engine_pause` の中で呼び手の thread からも呼ばれる（今の code の通り）ので media.h の comment にそう書いた。exports.map は `media_engine_*` の wildcard なので変更なし。engine.c の `<fcntl.h>`・`<unistd.h>` は使わなくなったので外した。
- page/media.c: static の関数は file の他の static と同じ `media_` の名前（`media_waker`・`media_wake`）。loader の無い page には wake を渡さない（NULL）。
- `media_engine_open` の他の利用者は無い（`grep -rn` で libbrowser の page/media.c の 2 か所だけ）。

未実施:

- zedBSD（QEMU・実機）の上の https と `<video>`（RTLD_LOCAL の確かめ、wake の道の画面での確かめ）。headless の `--render` は media の engine を追わないので、host の browser でも `<video>` の wake の道（page/media.c → net_loader_ring → view）は通していない（libmedia の wake と loader の ring はそれぞれ単体の試験で）。
- Linux の Keiland・FreeBSD の build、zedBSD arm64 の build、Windows の実機、aarch64 の Windows の実行。
- 4094 本を超える接続の EMFILE の試験。

他の WS への影響（Q1 へ）:

- `plan/ws314/tests/loader-test.sh`（WS314 completed）は基点の時点で既に build できない（`base/*.c` に `*-win32.c` が入り `<winsock2.h>` が無い）うえ、`net_loader_watch` を使う。tools.md・未完の Phase から参照されていないので削除の候補（watch の項を ring に替えた写しが `plan/ws334/tests/net-ring-test.c`）。
- WS318（planning）の design.md の libmedia の notify の案は「libbrowser は notify の中で pipe に書き `net_loader_watch` を向ける」としているが、watch は無くなったので `net_loader_ring` を呼ぶ形になる。
