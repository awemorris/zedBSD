<!-- awesome-plan project=zedbsd record=ws334-p021 -->

# ws334-p021 統合: libbrowser の OS に触れる所を OS の層へ（network を除く）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.2・§3.4・§9.1。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p005（os-system）・p006（os-file）・p008（os-time）・p020（sources.mk と Windows の build の第 1 段）。
- 担当の想定: Opus（phase-runner）を 1 回。module の約束は各 header（`base/os-*.h`）の comment で、この Phase は呼び出しを替えるだけ。挙動を変えない（zedBSD・Linux の host の結果は前と同じ）。

## 調整（始める前）

- 下の「変える file」のうち design §9.1 で P8 と重なりが中・高の物（`page/page.c`・`page/svg.c`・`view/view.c`・`bind/environment.c`・`css/values.c`・`page/script.c`）の一覧を Q1 に送り、P8 の作業と当たらない時に始める（Q1 の指示）。commit は file の群ごとに小さく分ける。

## 変える file と中身

| # | file:行（設計の時の行。main の今の行で探し直す） | 今 | 替える先（header の約束） |
| --- | --- | --- | --- |
| 1 | `base/file.c` 全体 | `open`・`read`・`write`・`close` | `wb_file_read` は `wb_os_file_read` を、`wb_file_write` は `wb_os_file_write` を呼ぶだけにする（関数の名前と約束は base.h のまま。`<fcntl.h>`・`<unistd.h>` を外す） |
| 2 | `vm/heap.c:555`・`:745` と解放の `:137`・`:570`・`:1005`・`:1067` | `posix_memalign`・`free` | `wb_os_aligned_alloc`・`wb_os_aligned_free`。aligned の block を放す全部の `free` を替える（他の `free` は替えない。どれが aligned の block かを読んで確かめる） |
| 3 | `page/svg.c:1968` | `memmem` | 新しい `wb_memmem`（`base/base.h` に宣言、`base/buffer.c` に ISO C の素直な実装: needle の長さ 0 は haystack を返す、無ければ NULL。`net/http.c` の `http_memmem` と同じ約束）。`http_memmem` は p022 で `wb_memmem` に替える |
| 4 | `page/page.c:284` | `realpath(path, NULL)` と strdup の予備 | `wb_os_full_path` を `wb_buffer` に。失敗の時は今と同じく path の写し |
| 5 | `view/view.c:436` | `getcwd` と "/" の足し | `wb_os_current_directory` で作業の directory を得て、その path を `net_url_from_file_path`（下の 6）で file: URL にし、末尾に "/" を足した URL を `page_resolve_location` の base にする（Windows の "C:\\x" も "file:///C:/x/" になり、相対の location が URL として解ける。`page_resolve_location` が location 自身の絶対の path を見分ける所は下の 17 の `wb_os_path_is_absolute`） |
| 6 | `net/url.c:412-459` `net_url_file_path`・`net_url_from_file_path` | host が空でなければ ENOENT、path をそのまま | `net_url_file_path` は percent-decode の後に `wb_os_path_from_url(url->host, decoded, out)`。`net_url_from_file_path` は `wb_os_url_from_path(path, &host, &decoded)` で分け、"file://" + host + 今の符号化（`%`・`#`・`?` と `URL_SET_PATH`）の path |
| 7 | `js/builtin_date.c:289`・`:1459`・`:1484`・`:1662` | `tzset`・`localtime_r`・`clock_gettime(CLOCK_REALTIME)` | `wb_os_time_zone_start`、offset は `wb_os_local_offset`（秒）× 1000、名前は `wb_os_zone_name`（buffer は `WB_OS_ZONE_NAME_MAX + 1`）を今の略号の表（:200-212）に通す（Windows の長い名前は表に当たらずそのまま）、今は `wb_os_wall_ns` を ms の double に。誤りの時は今と同じ（offset 0、名前 "UTC"） |
| 8 | `page/script.c:450`・`:468`、`view/view.c:2033` | `clock_gettime(CLOCK_MONOTONIC)` | `wb_os_monotonic_ns`（単位の換算を今の計算に合わせる） |
| 9 | `bind/environment.c:790` | `clock_gettime(CLOCK_REALTIME)` | `wb_os_wall_ns` |
| 10 | `js/builtin_typed.c:1377`、`bind/environment.c:1228` | `fopen("/dev/urandom")` | `wb_os_random`。失敗は今の失敗の扱いと同じ |
| 11 | `bind/environment.c:29`・`:38-49`・`:229` | `ZEDBSD_OS_NAME " " ENVIRONMENT_MACHINE` の static の文字列 | `wb_os_platform_name()`。表が static const の初期値なら、platform の行だけを表から外して、表を読む関数の中で `wb_os_platform_name()` を返す（`include/kern/os-name.h` の include と `ENVIRONMENT_MACHINE` は os-system-posix.c の側にある物を使い、ここから消す） |
| 12 | `net/net.h:25` `NET_USER_AGENT` | 固定の文字列 | `net_user_agent()`（`net/url.c` か新しい行を `net/http.c` に: "Mozilla/5.0 (" + `wb_os_user_agent_system()` + ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36" を once で組み立てて static に持つ）。`NET_USER_AGENT` を使う全部の所を替える（navigator.userAgent を含む） |
| 13 | `text/text.h:39-41`、`text/font.c:74`・`:80` | `KEILAND_DATADIR "/fonts/..."` の固定の path | 既定の font の path を `wb_os_data_directory` + "/fonts/keiland.ttf" などで実行時に組み立てる（`text_default_path(which, buffer)` の static の関数を font.c に）。zedBSD の値は今と同じ "/usr/share/fonts/..."。companion（bold・fallback_mono）は p023 の公開の `browser_fonts` で |
| 14 | `css/values.c:7012-7013` | `(long)number` で整数かの判定 | `number == floor(number)` と範囲（`fabs(number) < 9007199254740992.0`）の比較にする（LLP64 で 2^31 以上が未定義の動作になるため）。結果が今の 64 bit の host と同じであること |
| 15 | `libbrowser/sources.mk`・`Makefile` | — | `LIBBROWSER_OS_SOURCES := $(LIBBROWSER_DIR)/base/os-system-posix.c $(LIBBROWSER_DIR)/base/os-file-posix.c $(LIBBROWSER_DIR)/base/os-time-posix.c` を足し、この 3 つを `LIBBROWSER_SCRIPT_SOURCES` に入れる（script の core も時刻・乱数・file を使う。network の os-net-posix.c は p022 で `LIBBROWSER_ENGINE_SOURCES` に入れる: script の core は network を使わない）。Windows の物は sources.mk に書かない（`list-sources.sh` が拾うため）。`apps/browser-win32/Makefile` が `LIBBROWSER_SCRIPT_SOURCES` から posix の 3 つを除き、win32 の 4 つ（os-win32・os-system-win32・os-file-win32・os-time-win32）と os-zones.c を足す |
| 16 | `plan/ws074/tests/host-build.sh` | 全部の source を host の cc で | `list-sources.sh` が posix の 3 つを拾うので変更は要らない見込み。拾わなければ Q1 に（WS074 の file） |
| 17 | `page/link.c:458`・`page/script.c:358`・`page/media.c:518`（設計の時の行） | `[0] == '/'` で「local の path か」を見分ける | `wb_os_path_is_absolute`。`page/script.c:358` の `"file://%s"` の書き出しは `net_url_from_file_path`（符号化と Windows の drive・UNC を正しく）に替える。`page/page.c:284` の後の `page->base` に入るのは Windows で "C:\\x\\a.html" の形なので、この 3 か所が base を path と見分けられること |
| 18 | `page/page_resolve_location`（`grep -n page_resolve_location`、location の始まりが '/' かの判定） | `[0] == '/'` | `wb_os_path_is_absolute`（Windows の "C:\\x" を URL の scheme "c" と読まないため） |

- 触れた block に開発の管理情報（ws074-pNNN など）の comment があれば、この Phase では直さない（p035 の規約の見直しで）。新しく書かない。
- OS の header（`<unistd.h>`・`<fcntl.h>`・`<sys/*.h>`・`<dlfcn.h>`・`<pthread_np.h>`）の include を、使わなくなった file から外す。`net/` の 3 つ（loader・http・tls）と `page/storage.c` は p022・p023 で（ここでは触れない）。

## Windows の build

- `apps/browser-win32/Makefile` に `libbrowser-core` の target: `LIBBROWSER_SOURCES` から `net/loader.c`・`net/http.c`・`net/tls.c`・`page/storage.c`・`net/cookie.c`（p022・p023 の物）を除いた全部を x86_64 と aarch64 で compile（warning 0、`-Werror`）。生成の表（`build/` の下の物、`libbrowser/Makefile` の規則）は Windows の build でも同じ生成の規則を呼ぶ（host の python・tools、生成物は `$(BROWSER_WIN32_BUILD)/obj/gen/`）。
- `cross-build.sh` に libbrowser-core の `nm -u` の照合を足す（許す表に `wb_os_*` の Windows の側が呼ぶ system の関数と UCRT を足す）。

## 試験（受け入れ）

- 段 1（design §7.1）: 変える前（基点）と後で同じ件数: `timeout 3600 sh plan/ws074/tests/host-build.sh plain` と `asan` の host-*.c の PASS の数、`run-js-tests.py`・`run-test262-parse.py` の件数、`plan/tools/browser-component/run.sh plain asan`。前の件数は基点の tree で先に数える（同じ worktree で `git stash` は使わず、基点の別の worktree を Q1 に頼むか、`git worktree` を作らずに `git archive <基点> | tar -x -C build/tmp/p021-base` で展開して流す）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → PASS（module の試験が壊れていない）。
- `timeout 1800 sh plan/ws334/tests/cross-build.sh` → PASS（libbrowser-core が両方の arch で warning 0）。
- zedBSD amd64 の `build/amd64/dynamic/libbrowser.so` と `build/amd64/bin/browser`（p020 の command）→ warning 0。
- Wine の確かめ（Q1 の割当は要らない）: `libbrowser-core` の object と `js/tool.c` で作る小さな `script-tool.exe`（p025 の archive の前の簡易な link。`browser_script_tool` を呼ぶ main を `plan/ws334/tests/script-tool-main.c` に）で、`new Date(0).toString()`・`Date.now()` の桁・`crypto.getRandomValues` を Wine で流し、名前が Windows の長い名前（Wine の zone）になる。
- `git diff --check`。

## 変えてはいけない物

- module の header と .c、試験の道具（modules.sh・run-unit.sh・check-skeleton.sh・unit/）。header の不足を見つけたら止めて Q1 に。
- 表の 18 の外の libbrowser の file。network（p022）、storage（p023）。HAL・toolchain・共有の build/。rm をしない。push しない。

## 終わりの条件

- 上が全部 PASS。件数の前後の表を結果の節に。file の群ごとの commit（`git commit -m WIP -- <path>`）。SHA・command と最後の行・未実施を Q1 に。ws.md の行を「実装済み・試験 PASS」に。
