<!-- awesome-plan project=zedbsd record=ws334-p022 -->

# ws334-p022 統合: network を OS の層へ（loader・http・tls）と libmedia の起こしの callback

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.2 の O1〜O12・O25・O32・O33、§3.4、§5 の M1、§9.1。
- 状態: planned（2026-11-01 p002 で作成）。
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
