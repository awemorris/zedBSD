<!-- awesome-plan project=zedbsd record=ws334-p021 -->

# ws334-p021 統合: libbrowser の OS に触れる所を OS の層へ（network を除く）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.2・§3.4・§9.1。
- 状態: in-progress（2026-11-01 04:49 q1808、P7。2026-11-01 07:07 中断: host の再起動、下の「再開の点」）。
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

## 結果（2026-11-01、P7、q1808-i01、基点 98382fe76）

commit（`git commit -m WIP -- <path>`、file の群ごと）:

| SHA | 中身 |
| --- | --- |
| 4d451e9d3（main に e0e0dae6e で merge 済み） | P8 と重なる 6 つ（page/page.c・page/svg.c・view/view.c・bind/environment.c・css/values.c・page/script.c）と、それが build に要る物（base/base.h・buffer.c の `wb_memmem`、net/net.h・url.c の `net_user_agent`・file: URL、net/http.c の UA の 1 行、sources.mk の `LIBBROWSER_OS_SOURCES`） |
| 525bc577a | base/file.c・vm/heap.c・js/builtin_date.c・js/builtin_typed.c・page/link.c・page/media.c |
| 781ec96c8 | text/text.h・text/font.c（`text_default_path`）と view/view.c の 3 行 |
| 8c4fc75b4 | apps/browser-win32/Makefile（`libbrowser-core`、`flags` に `LIBBROWSER_CORE_OBJECTS`）、plan/ws334/tests/cross-build.sh（libbrowser-core の nm の照合）・allowed-imports.txt・script-tool-main.c |

前後の件数（基点と後の tree はどちらも `git archive` で `build/tmp/p021-base`・`build/tmp/p021-tip`（781ec96c8）に展開して流した。8c4fc75b4 は Windows の build と試験の道具だけ）:

| 確かめ | 前（98382fe76） | 後（781ec96c8。4d451e9d3 も同じ） |
| --- | --- | --- |
| `host-build.sh plain` | exit 0、warning 0、built 93 | 同じ |
| host-*（plain、各 600 s、exit 0 を PASS） | 75 PASS・16 exit 2（引数の要る試験） | 75・16、試験ごとの exit の値が全部同じ |
| `host-build.sh asan` と host-*（asan） | exit 0、warning 0、built 93、75・16 | 同じ（試験ごとに同じ） |
| `plan/tools/browser-component/run.sh plain` / `asan` | 83 checks PASS / 83 checks PASS | 同じ（4d451e9d3 の asan は 1 回目が負荷で 1800 s の時間切れ、3600 s でやり直して PASS） |
| `run-js-tests.py` | js-tests 17/17 | 17/17 |
| `run-test262-parse.py`（test262 7ab7fafa0、`fetch-suites.sh test262` で取得） | 46551/47792、positive 42772/43441、negative 3779/4351 | 同じ。失敗の一覧（1241 行）も一致 |

他の確かめ:

- zedBSD amd64（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=<写し> -o <写し>/.zedbsd-sysroot-complete -j16 build/amd64/dynamic/libbrowser.so build/amd64/bin/browser`）→ exit 0、warning 0。sysroot は p020 と同じく `build/tmp/p021/sysroot-amd64` に写し、tree の `include/libc`・`userland/desktop/include` を重ねた。
- `timeout 3000 sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（win64: compat 26 imports、libbrowser-core 126 imports、winarm64: 24・122、全部が表の中。遅延の import と Wine の 2 通りも PASS）。libbrowser-core は 189 の object（`LIBBROWSER_SOURCES` から posix の OS の層 3 つと net/loader・http・tls・cookie、page/storage を除く）が x86_64・aarch64 とも warning 0（`-Werror`）。
- Wine（`build/tmp/ws334-wine`、zone は host の Asia/Tokyo）: script の core（`LIBBROWSER_SCRIPT_SOURCES` の win32 の object）と Windows の OS の層と `script-tool-main.c` を `-static` で link した `script-tool.exe`（import は KERNEL32・CRYPT32・bcrypt・UCRT の api-ms-win-crt-* だけ、winpthreads は静的）: `new Date(0).toString()` → `Thu Jan 01 1970 09:00:00 GMT+0900 (Japan Standard Time)`、`getTimezoneOffset()` → -540、`Date.now()` は 13 桁の整数、`script-tool random` → 16 byte の乱数（2 回で違う値）。
- `git diff --check` → 出力なし。
- **未検証** `LC_ALL=C.UTF-8 timeout 3600 sh plan/ws334/tests/run-unit.sh all`: 1 回目（781ec96c8 の worktree）は `run-unit: FAIL`。Wine の 6 本は「wine server に繋げない」（cross-build.sh と別の Wine の試しを同時に流したため）、test-files の posix・win32 は負荷で時間切れ（exit 124）。2 回目を単独で流し、cf-html・files・frame・ico・input・keys-table・media-os の posix・win32 と os-file の posix の 15 本が PASS（FAIL 0）の所で、host の再起動のため止めた。残りの試験は未実施。p021 は module と試験の道具を変えていない。

自分で決めた点・phase.md と code の食い違い:

- #2 heap.c: aligned の block を放す `free` は表の 4 か所（:137・:570・:1005・:1067）に加えて :148（heap の解放の時の大きな cell `heap->large[item]`）もあり、5 か所全部を `wb_os_aligned_free` にした。
- #3 `wb_memmem`: phase.md の約束（needle の長さ 0 は haystack を返す）に従った。`http_memmem` は長さ 0 で NULL を返すので「同じ約束」ではない。p022 で替える時に注意（http は長さ 4 でしか呼ばないので差は出ない）。
- #5 view.c: 末尾の "/" は URL が "/" で終わっていない時だけ足す（root の "/" と "C:\\" で "file:////"・"file:///C://" にしないため）。root の作業 directory の時だけ前と違う（前は "file:////" の base）。
- #11 environment.c: 表の行を外すと navigator の prototype の property の順が変わるので、`{ "platform", NULL }` を残し、表を読む `environment_strings` が値 NULL の行に `wb_os_platform_name()` を使う形にした。
- #13 font: 既定の font の path（`TEXT_DEFAULT_*`）は今の code で font.c でなく `view/view.c:340-342` で使われている。`text_default_path(face)` を font.c の公開の関数（text.h に宣言）にし、data の directory + "/fonts/…" を一度（`pthread_once`）組み立てて program の間持つ。view.c の 3 行を替えた。zedBSD の値は "/usr/share/fonts/…" のまま。companion（font.c:74・:80 の `KEILAND_FONT_BOLD`・`KEILAND_FONT_FALLBACK_MONO`）は表の通り p023。
- #14 values.c: 範囲は phase.md の 2^53 でなく 2^63（`-9223372036854775808.0 <= x < 9223372036854775808.0`）にし、`%lld` で書く。「結果が今の 64 bit の host と同じ」を守るため（2^53 にすると 2^53〜2^63 の整数が `%.6g` に変わる）。
- #12 `net_user_agent` は net/url.c に置いた（`pthread_once` で組み立てる）。
- #6 `net_url_from_file_path` は絶対でない path に EINVAL を返すようになった（呼ぶのは link.c の、`wb_os_path_is_absolute` で確かめた後の 1 か所だけ）。
- #17 page/script.c の `page_url` は `"file://%s"` から `net_url_from_file_path` に替えたので、path の空白・`%`・`#`・`?` などが符号化される（前は `#` が fragment に読まれた）。
- #16 host-build.sh: 変更は要らなかった（list-sources.sh が posix の 3 つを拾う。1 行に 1 file で書いたため）。
- `wb_file_read`・`wb_file_write` は os-file の errno の写し（ELOOP→ENOENT など）と、UTF-8 でない path の EILSEQ になる（p006 の決め）。
- 表の 7（Date）: 年 1601〜9999 の外は os-time の clamp で端の年の offset になる（p008 の決め）。

未実施:

- Wine の別の zone（`TZ=America/New_York`）で Windows の長い名前の道（表の key → 名前）を見る確かめ: 新しい prefix を 2 つ作って試したが、host の負荷で prefix の作成が 300 s で切れた物と、`could not load kernel32.dll` になった物で、流せていない。Tokyo の "Japan Standard Time" は POSIX の略号の表と Windows の表の両方で同じ文字なので、Windows の道を通ったことの区別にはならない。
- `crypto.getRandomValues` を Wine で JavaScript から呼ぶこと（script の core だけの realm に crypto が無い。`wb_os_random` を同じ object から直に呼んで確かめた）。
- 実機の Windows、aarch64 の Windows の実行（compile と link の表だけ）、zedBSD arm64・Linux の Keiland・FreeBSD の build。

## 再開の点（2026-11-01 07:07、host の再起動で中断）

- main に入った物: 4d451e9d3（e0e0dae6e で merge 済み）。merge を待つ物（検証済み、この順）: 525bc577a、781ec96c8、8c4fc75b4。この節の記録は別の commit。
- 残り 1（p021 の受け入れ）: 他の Wine と並べずに単独で `cd /home/awe/zedBSD-worktrees/p7-ws334 && LC_ALL=C.UTF-8 timeout 3600 sh plan/ws334/tests/run-unit.sh all > build/tmp/p021/run-unit-3.log 2>&1; tail -1 build/tmp/p021/run-unit-3.log`。`run-unit: PASS` なら上の「未検証」の行を直し、ws.md の p021 の行を「実装済み・試験 PASS」にして Q1 に。
- 残り 1 の任意: Wine の別の zone の確かめ（`build/tmp/ws334-wine` の wineserver が止まっている時に `WINEPREFIX=$PWD/build/tmp/ws334-wine TZ=America/New_York WINEDEBUG=-all timeout 600 wine build/tmp/p021/win64/script-tool.exe build/tmp/p021/win64/date.js`。script-tool.exe と date.js は `build/tmp/p021/win64/` にある。無ければ上の結果の節の link の形で作り直す）。Linux の host の答えは "Eastern Standard Time"・"Eastern Daylight Time"、offset 300。
- 残り 2（p021 の後、同じ Queue の追い、p021 の commit に混ぜない。Q1 の 2026-11-01 の指示）:
  1. 単一の .exe（DLL を作らない。libbrowser・libmedia・libkeiland・compat・winpthreads・当面の OpenSSL を静的に、vulkan-1.dll は遅延の import のまま）: design.md §6（§6.1 の winpthreads の注、§6.2 の配る形・DLL の一覧・export（Windows では .def・exports.map が要らない）、§6.6・§6.8 の OpenSSL の静的な library と license の文）を直す。WS335 の注（main 8e44fa17e、plan/ws335/design.md §4.2・§4.4）も §6 に: 切り替えの時は WS335 の 2 つの sources.mk で libssl-compat.a・libcrypto-compat.a と -lbcrypt -lcrypt32 -lws2_32 を link、OpenSSL とその license の文を外す、ca-certificates の束を .exe の隣の cert.pem に、SSL_set_fd の前に WSAStartup、Zlib の license の文。p030 の前に net/tls.c の表の OpenSSL だけの項（p030 が足す証明書ごとの検証の callback など）を任意にする。
  2. ws.md に p035 の前の新しい Phase の行（次の空き番号）を足し、`apps/browser-win32/Makefile` と build の glue で `make browser-win64` が 1 つの .exe を作る形にし、`llvm-readobj --coff-imports`（か `objdump -p`）で system の DLL だけを import することを確かめ、win32 の単体の試験を Wine で流す。
  3. libkeiland の Windows は pthread を使わない（2026-11-01 ユーザーの決定、ws.md）: `platform/platform.h` の `<pthread.h>`・`pthread_mutex_t`・`pthread_t`、`platform-win32.c` の `pthread_mutex_*`、sources.mk の `thread-posix.c` を Windows では SRWLOCK か CRITICAL_SECTION・GetCurrentThreadId・CreateThread に（keiui_* の約束はそのまま）。win32 の libkeiland の object が pthread の symbol を参照しないことを `llvm-nm` で確かめる。上の 2 と同じ新しい Phase か、p035 の前の別の Phase の行に。libbrowser・libmedia は当面 winpthreads を静的に（CreateThread への書き換えは Future Work F-WS334-win32-threads、この Queue ではない）。
