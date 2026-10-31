<!-- awesome-plan project=zedbsd record=ws334-design -->

# WS334 設計: OmniGrammaton（libbrowser の Web ブラウザ）の Windows への移植

- 親: [WS334](ws.md)（ws334-p001、2026-11-01 P7、q1787-i01）。ユーザーの決定は ws.md の 1〜15（要点）。Q1 の覚え書きは [design-notes.md](design-notes.md)。
  決定 12〜15（`make browser-win64`、tree の上の `apps/browser-win32/`、後の `apps/browser-qt`・`apps/browser-gtk4`、libkeiland はまず browser に要る部分だけ、UI は libkeiland の上の今の UI のまま）は設計の途中で受けた。§4 と §6.9 はそれに合わせた形。
- 読んだ物: AGENTS.md、plan/agents/protocol.md、plan/master-design-policy.md（§2.6 interface と遅い抽象、§11）、plan/guardrail.md（範囲・出典）、
  plan/standards/browser-component.md、plan/ws327/design.md（§2・§5）、plan/ws327/ws.md、plan/future-work.md（F-WS327-win32-build）、
  plan/ws234/ws.md（保留中の libkeiland の DLL 版）、docs/agent/spec-module-delegation.md、そして下の §2 に挙げる source。
- source の行の番号は worktree `/home/awe/zedBSD-worktrees/p7-ws334` の 26000e286（main 380c66ab9 + ws.md の決定 11）の物。path は特に書かない限り
  `userland/desktop/` から。libkeiland・shell・libmedia の一部の行は調べの補助（Explore の担当 2 本）の報告から取り、主な物は P7 が grep で照合した（§2.12）。
- この文書は code を書かない。HAL・kernel に触れる所は無い（`plan/ws334/proposed/` は作らない）。toolchain は変えず、案だけを §6.1 に置く。

## 0. 要約

| 項目 | 推し（問いは [user-questions.md](user-questions.md)） |
| --- | --- |
| libbrowser の OS の層 | libbrowser の中に private の OS の層 `base/os.h` と `base/os-posix.c`・`base/os-win32.c` を足し、socket・poll・起こし・file・path・時刻帯・乱数・library の読み込みをそこへ集める。thread の基本（mutex・cond・create）は mingw の winpthreads を使い、書き換えない（§3.4）。libkeiland の `kl_thread`・`kl_socket` は使わない（WS327 の決定 2） |
| 3 つの engine（決定 9・10） | レンダリング（dom・html・xml・css・layout・paint・text・image）とスクリプト（vm・js）の分離は今の tree の通り保つ（DOM を出さない）。スクリプトの core（base・vm・js）を単独の archive として build できる事を試験で保証する。ストレージバックエンドは**新しい公開の interface**（`struct browser_storage_calls`）で、新しい公開の `struct browser_instance` に登録する（§3） |
| instance | `browser_instance` が cookie の jar・HTTP の cache・Web Storage の領域とストレージバックエンドを持つ。view は instance の中に作る。`BROWSER_API_VERSION` 3 → 4（§3.8） |
| browser の UI と libkeiland（決定 3・8・13・14・15） | 決定 15 で案 A: browser の UI（今の `userland/desktop/browser/`）は 1 つで、どの OS でも同じ source。OS と toolkit の差は libkeiland の backend（Wayland・Win32、将来 Qt6・GTK4）が吸収する。`apps/browser-win32/`（将来 `apps/browser-qt`・`apps/browser-gtk4`）は入口・resource・build だけ。libkeiland の Win32 の backend は Win32 直で、まず browser に要る最小の部分（§4.5）、Windows の口の 9 項目（§4.4）のうち snap・IME の composition・DPI・clipboard・pointer・暗い mode を最初に。Win32 を直に使う版（案 B）は libkeiland で足りない時の予備（§4.2） |
| libkeiland の全体の移植 | browser の質に効く口（TSF・drag と drop・DirectManipulation・accent・UIA）は WS334 の後半、browser の使わない API の全部は WS234（§4.9） |
| build（決定 11・12） | 一番上で `make browser-win64`（x86_64）・`make browser-winarm64`（arm64）。`apps/browser-win32/Makefile` を呼び、出力は `build/browser-win64/`・`build/browser-winarm64/`。toolchain は llvm-mingw を `LLVM_MINGW` か PATH で探し、x86_64 は Debian の mingw-w64 gcc も使える。UCRT（§6.1・§6.9） |
| 試験 | 初めは host の cross build の warning 0 と、Linux の host での既存の試験（OS の層の posix の側）。shell がある程度できたら実機の Windows（ユーザーの host）で。Wine は任意で、Vulkan の判定に使わない（決定 11、§7） |
| Phase | p002〜p018 の 17 Phase、計 約 131 LW（§8、design-reviewer の指摘の後）。libbrowser の変更は §9 の file の一覧に限る |

## 1. 範囲

- 範囲の中: libbrowser の platform に依る所の分離、3 つの engine の interface（ストレージバックエンドの新設を含む）、libmedia の Windows で合わない所、
  libtruetype と compat の library の Windows の build、shell の共通の部分と toolkit ごとの層への分け方（`apps/`）、Win32 の frontend、
  libkeiland の Win32 の実装の位置付け（決定 3・8、§4.6）、`make browser-win64`・`browser-winarm64`、
  .exe・DLL・resource・名前・TLS・証明書・font・配布の形、試験、Phase の分け方。
- `apps/browser-qt`・`apps/browser-gtk4` は将来の入口と build（決定 13・15: libkeiland の Qt6・GTK4 の backend で同じ UI を動かす）。WS334 は作らない。
- 範囲の外（理由）: accessibility（libbrowser にも libkeiland にも accessibility の木が無い。§4.4、Future Work の候補）、
  高 DPI の engine の対応（libbrowser の layout と paint に device scale が無い。WS074 の範囲に関わる。§4.5、問い）、
  sandbox・process の分離（今の libbrowser は 1 process。§12 の危険）、自動更新、installer の署名（問い）、
  libkeiland の全部の API の Win32 化（browser の使わない API は WS234 の範囲。WS334 は browser に要る最小の部分と browser の質に効く Windows の口、§4.4・§4.9）。

## 2. 事実（code から読んだ物）

### 2.1 libbrowser の構成（`libbrowser/`、16 の directory、約 24 万行）

| dir | 行 | 中身 | 他の dir への include（数） |
| --- | --- | --- | --- |
| base | 5,325 | arena・buffer・vector・utf・hash・file・unicode | base だけ |
| vm | 24,089 | GC の heap・string・atom・object・interpreter | vm(25)・base(1) |
| js | 45,162 | lexer・parser・compiler・builtins・module・tool | js(39)・vm(4)・base(4) |
| dom | 6,163 | node の木 | dom(12)・vm(1)（`dom/dom.h:22` が `vm/vm.h`） |
| html | 11,765 | tokenizer・parser | html(9)・dom(1)・base(1) |
| xml | 2,145 | XML の parser | xml(3)・dom(1)・base(1) |
| css | 29,765 | tokenizer・parser・cascade・値 | css(13)・dom(1) |
| layout | 24,261 | box・block・inline・flex・grid・table | layout(22)・text・paint・image・css |
| paint | 7,737 | display list・software・vulkan | paint(6)・layout(1) |
| text | 1,338 | font・linebreak・woff | text(3)・base(1)・`userland/desktop/paths.h`（`text/text.h:28`） |
| image | 666 | decode・jpeg・png・gif | image だけ |
| bind | 55,359 | DOM の binding（script から見える page） | bind(48)・css(9)・net(4)・js(3)・html(3)・dom(1)・`include/kern/os-name.h`（`bind/environment.c:29`） |
| net | 9,593 | url・host・data・http・tls・cookie・loader | net(7)・base(2) |
| page | 18,661 | 1 文書の統合（script・storage・media・input…） | page(17)・net(11)・bind(3)ほか全部・`libmedia/media.h`（`page/media.c:31`） |
| view | 3,531 | 公開の view（`browser_view_*`） | paint・page・net |
| tools | — | 表の生成の python（build の外） | — |

- 公開の header は `include/browser/browser.h`（484 行、`BROWSER_API_VERSION 3` は :104）。export は `libbrowser/exports.map`（`browser_view_*`・
  `browser_offscreen_*`・`browser_add_ca_file`・`browser_script_tool`）。
- build は zedBSD だけ: `libbrowser/Makefile:213` の `ZEDBSD_USERLAND_PACKAGE`（依存は libtruetype・libvulkan・libmedia・libjpeg/png/gif-compat・libz-compat）。
  Linux・FreeBSD の host の build（`keiland-linux.mk`・`keiland-freebsd.mk`）は libbrowser・libmedia・browser を持たない。
  host の試験の build は `plan/ws074/tests/host-build.sh`（WS074 の物。全部の source を host の cc で compile、`-lvulkan -lm -ldl -lpthread`）。

### 2.2 libbrowser の中で OS に触れる所（全部）

grep の語: `#include <` の OS の header（unistd・pthread・poll・sys/socket・netdb・fcntl・sys/stat・dlfcn・strings・pthread_np）、`getenv`・
`clock_gettime`・`localtime_r`・`/dev/urandom`・`realpath`・`getcwd`・`getpid`・`mkdir`・`rename`・`unlink`・`#if defined(__linux__|__FreeBSD__)`。
ISO C の `<stdio.h>`・`<time.h>` の `time` などは除いた。

| # | file:line | 使う物 | Windows での扱い（§3.4 の OS の層の関数） |
| --- | --- | --- | --- |
| O1 | `net/loader.c:61-77` | fcntl・netdb・poll・pthread・sys/socket・unistd・`pthread_np.h`（FreeBSD）、`#if defined(__linux__) \|\| defined(__FreeBSD__)` の thread の名前 | include を OS の層へ。thread の名前は `wb_os_thread_name` |
| O2 | `net/loader.c:394`・`:406` | `pipe2(O_CLOEXEC\|O_NONBLOCK)` 2 本（resolver の答えと制御の起こし） | `wb_os_waker`（POSIX は pipe、Win32 は loopback の TCP の組。WS327 §5.2 と同じ考え） |
| O3 | `net/loader.c:418`・`:426`・`:887`・`:899`・`:929` | resolver と network の 2 thread、`getaddrinfo`、pipe への `write` | thread は winpthreads のまま。`getaddrinfo` は WinSock にもある（host は punycode で ASCII）。`write` は waker へ |
| O4 | `net/loader.c` の poll の loop（`net_loader_watch` :736 の fd を含む） | `poll` で接続・pipe・watch の fd を待つ | `wb_os_poll`（POSIX は poll、Win32 は select、WS327 §5.2 の理由） |
| O5 | `net/loader.c:381`・`:661`・`:845` | `pthread_condattr_setclock(CLOCK_MONOTONIC)`・`clock_gettime` | winpthreads に `pthread_condattr_setclock`・`clock_gettime` がある（`/usr/x86_64-w64-mingw32/include/pthread.h:403`・`pthread_time.h:92`）。MONOTONIC の cond が効くかは未確かめ（p004 の最初に試す） |
| O6 | `net/http.c:26-36`・`:617-629`・`:681-709`・`:740`・`:777`・`:828` | getaddrinfo・socket・connect・close・`send(MSG_NOSIGNAL)`・poll・recv・`strings.h` | `wb_os_socket_*`・`wb_os_poll`。`MSG_NOSIGNAL` は Win32 に無い（SIGPIPE も無い） |
| O7 | `net/tls.c:32`・`:44-47`・`:687-717` | `dlopen("libssl.so"...)`・`dlsym` | `wb_os_library_open`・`_symbol`。名前は OS ごと（Win32 は `libssl-3-x64.dll`・`libcrypto-3-x64.dll`、arm64 は `-arm64`） |
| O8 | `net/tls.c:84`・`:240`・`:383` | `SSL_set_fd(ssl, int)` | Win32 の `SOCKET` を int に入れて渡す（OpenSSL の Windows の流儀）。socket の値を `wb_os_socket` 型（`intptr_t`）で持ち、範囲を確かめて渡す |
| O9 | `net/tls.c:19`・`SSL_CTX_set_default_verify_paths` | 既定の根の証明書（`/etc/ssl/cert.pem`） | Win32 は §6.6（Windows の根の store を読み込む） |
| O10 | `net/tls.c:158`・`:172-174`・`:899-930`、`net/cookie.c:28`・`:58` | pthread の mutex・key・once | winpthreads のまま |
| O11 | `net/http.c:70-77`、`bind/prelude.c:921`・`:1078`、`js/builtin_global.c:106-110`、`page/script.c:1790` | `getenv` の試験・調べの切り替え（BROWSER_NET_DUMP など） | Win32 の `getenv` で動く（ANSI の環境。UTF-8 の値は要らない） |
| O12 | `net/net.h:25` | User-Agent `"Mozilla/5.0 (X11; Linux x86_64) ..."` | OS ごとの定数を OS の層から（Win32 は `Windows NT 10.0; Win64; x64`、arm64 も同じ文字列が慣例）。問い U8 |
| O13 | `net/url.c:412-459` | file: URL ↔ local の path（`/` で始まる POSIX の path） | `wb_os_path_from_url`・`wb_os_url_from_path`（Win32: `/C:/x` ↔ `C:\x`、`\\host\share` ↔ `file://host/share`） |
| O14 | `base/file.c:15-17`・`:37`・`:91` | `open`・`read`・`write`・`close` | `wb_os_file_*`（Win32 は UTF-8 → UTF-16 の `_wopen`、`O_BINARY`） |
| O15 | `page/page.c:284` | `realpath` | `wb_os_full_path`（Win32 `GetFullPathNameW`） |
| O16 | `view/view.c:52`・`:436` | `getcwd` | `wb_os_current_directory` |
| O17 | `view/view.c:2033`、`page/script.c:450`・`:468` | `clock_gettime(CLOCK_MONOTONIC)` | winpthreads の `clock_gettime` で動く。OS の層に `wb_os_monotonic_ns` を置き、新しい呼び出しはそれを使う（今の行は変えない） |
| O18 | `page/storage.c:33-34`・`:663-667`・`:691-720`・`:872-881` | XDG の path、`mkdir`・`getpid`・`rename`・`unlink` | ストレージバックエンドへ移す（§3.5）。Win32 の `rename` は既にある file を上書きしない（`MoveFileExW(MOVEFILE_REPLACE_EXISTING)` が要る） |
| O19 | `js/builtin_date.c:16`・`:289`・`:1459`・`:1662` | `tzset`・`localtime_r` の `tm_gmtoff`・`tm_zone` | Win32 の `struct tm` に `tm_gmtoff`・`tm_zone` は無い。`wb_os_local_offset(time)`・`wb_os_zone_name(time)`（`SystemTimeToTzSpecificLocalTimeEx` と `GetDynamicTimeZoneInformation`） |
| O20 | `js/builtin_date.c:1484`、`bind/environment.c:790` | `clock_gettime(CLOCK_REALTIME)` | O17 と同じ |
| O21 | `js/builtin_typed.c:1377`、`bind/environment.c:1228` | `fopen("/dev/urandom")`（crypto.getRandomValues・randomUUID） | `wb_os_random`（Win32 `BCryptGenRandom` の system の RNG） |
| O22 | `js/builtin_math.c:542` | `time(NULL)` で Math.random の種 | ISO C で動く。そのまま |
| O23 | `bind/environment.c:29`・`:38-49`・`:229` | `navigator.platform` = `ZEDBSD_OS_NAME " " 機械名`（"zedBSD x86_64"） | OS の層の名前（Win32 は "Win32"。Chrome の Windows の値）。問い U8 |
| O24 | `text/text.h:28`・`:39-41`、`text/font.c:74`・`:80` | 既定の font の絶対の path（`KEILAND_DATADIR` = `/usr/share`、`paths.h:36-48`） | 公開の `browser_fonts` に bold と fallback_mono を足し、既定は OS の層の data の directory（Win32 は .exe の隣の `share\fonts`）（§3.8） |
| O25 | `page/media.c:132`・`:698`、`net/loader.c:736` | libmedia の wake の fd を loader の poll で待つ | wake を callback に変える（§5、O4 の fd の watch は不要になる） |
| O26 | `vm/heap.c:59`・`:270`、`vm/vm.h`・`vm/bytecode.h`・`vm/interpreter.c` | `__attribute__((noinline))`・`__builtin_unwind_init`・`__builtin_frame_address`（保守的な stack の走査） | gcc・clang の拡張。mingw の gcc と clang で使える。MSVC は対象にしない（§6.1） |
| O27 | `js/internal.h:17`、`js/compile.h:30`、`image/jpeg.c:21` | `setjmp`・`longjmp` | mingw の x64 の `setjmp` は既定で SEH の巻き戻しをする。全部の frame に unwind の情報があれば正しい（gcc・clang は付ける）。p003 で確かめる（§12 R4） |
| O28 | `bind/window.c:491`・`:516`、`bind/prelude.c:922`、`page/module.c:477`、`js/tool.c:110`ほか | `strerror` の文（console と標準の誤りの出力） | errno の名前は mingw の `errno.h` にあるが値が違い（POSIX の補いの値）、UCRT の `strerror` は EPROTO などを「Unknown error」と書く見込み（未確かめ）。挙動は変えない。試験が誤りの文を比べる所（runner の照合）は Windows で差になりうる → p016 で runner の期待を見る |
| O29 | 標準の出力の text の mode（`--dump`・`--run`・`--js` の出力、`browser/main.c` の `printf`・`fwrite`） | Windows の CRT は text の mode で `\n` を `\r\n` にする | `omnigrammaton-cli.exe` の入口で標準の出力と誤りの出力を binary の mode（`_setmode(_fileno(stdout), _O_BINARY)`）にする（`apps/browser-win32/main.c`）。golden の比較を zedBSD と同じ bytes にするため |
| O30 | `vm/heap.c:555`・`:745`（解放は `free`、`:137`・`:570`・`:1005`・`:1067`） | `posix_memalign` | mingw・UCRT に無い。`_aligned_malloc` は `_aligned_free` で放す。`wb_os_aligned_alloc`・`wb_os_aligned_free` を OS の層に足し、vm/heap.c の 6 か所を替える（レビュー #1） |
| O31 | `page/svg.c:1968` | `memmem`（GNU の拡張） | mingw に無い。`base/` に ISO C の `wb_memmem` を置く（`net/http.c:1348` の `http_memmem` と同じ働き）。OS の層でなく base の関数 |
| O32 | `net/tls.c:302`・`:482` | `SSL_ERROR_SYSCALL` の後に `errno` を読む | WinSock の誤りは `errno` に入らない。OS の層の「最後の socket の誤り」（`wb_os_socket_error`、Win32 は `WSAGetLastError` を errno に写す）を読む |
| O33 | `net/tls.c:36`、`net/net.h:184-186` | `POLLIN`・`POLLOUT` を `short *wants` で返す | `wb_os_poll` の印（READABLE・WRITABLE）に替える（net.h の内部の約束の変更） |
| O34 | `css/tokenizer.c:793`、`css/values.c:1841`、`page/svg.c:848`、`bind/svg-length.c:893` | `strtod`（locale の小数点に従う） | UCRT の既定は "C" の locale で今は同じ。libbrowser を使う program が `LC_NUMERIC` を変えると壊れる（Qt・GTK の backend は `setlocale` を呼ぶ）。約束「libbrowser を使う program は `LC_NUMERIC` を "C" のまま」を browser.h に書き、将来 locale に依らない読みに替える（R13） |

### 2.3 既に OS に依らない所

- base（file.c を除く）・vm（GC の stack の走査は O26 の拡張と、O30 の `posix_memalign`）・js（O19〜O22 を除く）・dom・html・xml・css（O34 の `strtod` の locale）・layout・paint・
  image は ISO C と `<math.h>`・`<setjmp.h>` だけ（page の O31 の `memmem` を除く）。§2.2 の grep は header の名前で探したので、標準の header に宣言のある POSIX・GNU の関数を
  漏らした（レビュー #1・#5）。p002 の `cross-build.sh` は mingw の object の `nm -u` を許す symbol の表と照合し、漏れを機械で見つける。Wayland・Keiland の窓に触れない（browser-component の規則 4、`plan/standards/browser-component.md`）。
- paint の Vulkan は拡張を使わない（`paint/vulkan.c:1879-1936` の `paint_offscreen_open` は拡張も layer も無く `VK_API_VERSION_1_0`）。shader は
  SPIR-V を `paint/shaders.h` に埋めてあり、走らせる時に compile しない。device・queue は呼び手の物（`paint/gpu.h:46-48`、`browser.h:213-234`）。
- image は libpng-compat を memory から（`image/png.c:42`）、gif は `DGifOpen` の callback（`image/gif.c:61`）、jpeg は `jpeg_mem_src`（`image/jpeg.c:73`）。
- font の本体は memory から開く（`text/font.c:210`・`:797` の `truetype_open(data, length, 0, ...)`）。companion（bold・fallback mono）だけが
  libtruetype に path を渡す（O24）。

### 2.4 engine の今の分かれ方（決定 10 の確かめ）

```
           html_script_hook                     bind_host・bind_storage_calls
 html/ ────────────────────▶ page/ ◀──────────────────────────────── bind/
 dom/ css/ layout/ paint/      │ (統合: 文書・loader・script・storage・media)  │ js_* (js/js.h)
 text/ image/  ── vm の heap・string・atom ──┐                               ▼
                                             └──────────────────────── vm/ js/ (script の core)
```

- **rendering → script**: html の parser は script の要素を hook で返すだけ（`html/html.h:165` `html_script_hook`、`:185` の登録。`html/parser.c:368`）。
  dom・css・layout・paint は bind・js の関数を呼ばない（grep で 0 件）。
- **script → rendering**: bind（DOM の binding）は `struct bind_host`（`bind/bind.h:224-300` の callback の表）を通して page に頼む。
  Web Storage は `struct bind_storage_calls`（`bind/bind.h:132-139`）。page がそれを満たす（`page/script.c:408` `host.storage = page_storage_calls()`、
  `:428` `bind_window_create`）。
- **script の core**: vm と js は dom・bind・page・net を include しない（§2.1 の表）。js の公開の口は `js/js.h`（parse・compile・run・module・builtins）。
- **共有の土台**: dom・html・css・layout・paint は vm の GC の heap と string・atom を使う（`dom/dom.h:22`、dom の `vm_heap_mark` 52 回・
  css の `vm_string_at` 37 回など）。DOM の node は vm の cell（`struct vm_cell cell`、`dom/dom.h:270`）。つまり rendering の engine は
  「vm の heap と string」を土台に持ち、js の compiler と bind は持たない。決定 10 に従い、この形を保つ（DOM を別の部品にしない）。

### 2.5 今の cookie・cache・保存（Q1 の覚え書きを source で照合）

| 物 | 所 | 持ち主と寿命 | thread |
| --- | --- | --- | --- |
| cookie の jar | `net/cookie.c:50-58`（static の配列 512 個、`COOKIE_MAX` :32） | process に 1 つ。file に保存しない。Expires・Max-Age は過去の時の削除だけ（:16-17） | page の thread（`page/script.c:1612`・`:1639` の document.cookie、blocking の fetch）と loader の network の thread（`net/http.c:426`・`:1010`）が `cookie_lock` で共有 |
| HTTP の cache | `net/loader.c:102`（`LOADER_CACHE_MAX` 64 MiB）・`:2300-2460` | loader ごと = view ごと（`page/network.c:48`、`view/view.c:377`）。memory だけ | network の thread |
| sessionStorage・localStorage | `page/storage.c:8-24`・`:82`（`storage_areas` は process の static） | 領域は process に 1 組。local は `$XDG_DATA_HOME/keiland/browser/local-storage/<origin の hex>.ls` へ変更のたびに書く（`:663-667`・`:838-890`、形式は `STORAGE_MAGIC` :39） | page の thread だけ |
| 他 | IndexedDB・Cache API・HSTS・履歴の保存・:visited の保存は無い（grep） | — | — |

### 2.6 browser の shell（`browser/`、5,974 行）

- main.c（931）: 引数・headless の mode（`--dump`・`--run`・`--render`・`--render-gpu`・`--js`、:11-18）。窓の mode は shell/ へ。
  OS の直の呼び出しは `fopen`（:590）と `__builtin_frame_address`（:197-203）だけ。home page は固定（:67）。
- shell/（4,861）: 窓（window.c）、Vulkan の表示（present.c）、titlebar（titlebar.c）、鍵の名前（keys.c）、touch（touch.c）、mail（mail.c）。
  - 使う libkeiland: `kl_app_open`・`kl_app_window_create(KL_PRESENT_NONE)`・`kl_app_dispatch`・`kl_app_take`・`kl_app_post`（network の thread の
    wake から、`shell.c:991-1000`）・`kl_window_size`・`_set_title`・`_close`、key・pointer・wheel・touch・focus の event（`window.c:228-291`）、
    text input（`kl_window_text_input`・`_text_cursor`・`_text_context`、`shell.c:613-634`）、copy（`kl_window_copy`、`mail.c:406`）、drag の絵
    （`kl_window_start_drag_icon`・`_drag_fill`、`shell.c:1153`・`:1172`）、titlebar の controls（`kl_window_set_controls` ほか、`titlebar.c:74-349`、
    compositor の `kl_titlebar_v1` が描く）、system（`kl_app_system`・`kl_system_mail_listen`・`_notify`、`mail.c:74-415`）、
    `kl_window_vulkan_surface`（`present.c:82`）、`kl_gesture_*`・`kl_scroller_*`（touch.c）。
  - 使わない: menu・file の選び・設定・`kl_thread`・`kl_socket`・font・timer。
  - Vulkan: shell が instance を `VK_KHR_SURFACE` と `VK_KHR_WAYLAND_SURFACE` で作る（`present.c:64-65`）→ device・queue を選び（:319-396）、FIFO の
    swapchain（:400-504）→ `browser_view_set_gpu`（:115-119）→ 毎 frame `browser_view_record`（:214-220）。CPU で描く代わりの道は無い。
  - 鍵: event の code は evdev の番号で、shell が US 配列の表で DOM の名前に変える（`keys.c:9-13`・`:27-39`）。
  - mail: system が無い時は code の機能なしで続く（`mail.c:72-79`）。
  - 名前: `app_options.application = "browser"`（`window.c:78`）、Vulkan の `pApplicationName = "browser"`（`present.c:68`）。
- 依存: `browser/Makefile:25`（libbrowser・libvulkan・libwayland・libkeiland・libz-compat、`picture/png-write.c` を一緒に compile）。

### 2.7 libkeiland と backend

- libkeiland（約 74,200 行、公開の header 34 個 約 7,700 行）に **window system の継ぎ目（backend の表）は無い**。`struct kl_window`（`libkeiland/ui/window.h:133-499`）が
  `wl_display`・`xdg_toplevel`・`wl_seat`・`zwp_*`・`Vk*` を直に持ち、window.h を 14 の .c が include する。dispatch は `wl_display_prepare_read` と
  `poll`（`ui/app.c:251-318`）。
- 公開の header にも Wayland の型が出る（`kl_app_display` が `wl_display *` を返す `keiland_app.h:159`、`kl_window_display`・`_surface`・`_seat`
  `keiland_window.h:236-242`、`kl_settings_open` など 11 の関数の引数）。前方宣言（`struct wl_display;`、`keiland_instance.h:21`）なので Windows でも compile は通る。
- OS に触れる所: `platform/platform.h:21`・`:55`（`keiui_thread_id` = `pthread_t`）、`platform-posix.c`（pipe の起こし・lock）、`process-posix.c`、
  `thread-posix.c`・`socket-posix.c`、そして WS327 の `thread-win32.c`・`socket-win32.c`（Makefile に無く、`plan/tools/keiland-layers/build-win32.sh:17-41` が
  mingw で compile と link だけをした）。platform/ の外の 19 の file が unistd・poll・mman・socket を include する（`ui/app.c`・`ui/clipboard.c`・
  `ui/present-shm.c` の mmap ほか）。
- 描画: `KL_PRESENT_VULKAN`（CPU の canvas を Vulkan の swapchain へ写す、`ui/present.c:8-28`）・`KL_PRESENT_SHM`（`wl_shm`、`ui/present-shm.c`）・
  `KL_PRESENT_NONE`（app が自分の Vulkan で描く。browser はこれ）（`keiland_window.h:61-63`）。
- font: libtruetype、fontconfig 無し（`ui/text.c:8-26`）、path は `KEILAND_DATADIR` の固定。IME: `zwp_text_input_v3`（`ui/text-input.c`）。
  clipboard: `wl_data_device`（`ui/clipboard.c`）。file の選び: 自前の窓（`ui/chooser.c`、`wl_shm`）。DPI: `wl_output` の scale を使わない（`ui/window.c:2947-2956`）。
- **libkeiland-backend**（`libkeiland-backend*`）は compositor だけが使う（`libkeiland-backend/keiland-backend.h:8-17`、guardrail の「配置」）。
  app の側の Windows の移植には関係しない。

### 2.8 libmedia と Vulkan Video

- 89 file、44,633 行 + mediafile（9,994 行）。libbrowser が使うのは engine だけ（`page/media.c` の `media_engine_open`・`_play`・`_pause`・`_seek`・
  `_status`・`_picture`・`_redraw`・`_close`・`media_engine_wake_fd`）。GPU の絵の書き出し（`media_decoder_gpu_pictures`）は使わない。音は出さない（`media.h:17-20`）。
- Vulkan Video: libmedia が自分で instance・device・queue を作る。loader は `dlopen("libvulkan.so.1")`（`vkvideo-runtime.c:1361-1363`）・
  `dlsym("vkGetInstanceProcAddr")`（:1379・:1433）、`VK_NO_PROTOTYPES`（:20）。device の拡張は `VK_KHR_synchronization2`・`VK_KHR_video_queue`・
  `VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`（:1736）、GPU の絵を出す時だけ `VK_KHR_external_memory_fd`（:1879-1881）。H.264 だけ。
  decoder の順は Vulkan Video → software の H.264（`h264sw-*`）（`decoder.c:97-101`）。zedBSD 固有の拡張・ioctl・dma-buf は無い。
- OS に触れる所: `engine.c:206-219`（`pipe`・`fcntl(F_SETFL)`）・`:259`・`:1018`（read・write）・`:794-802`・`:1029`・`:1043`（clock・cond・nanosleep）、
  pthread（99 か所、`engine.c`・`picture.c`・`aac-*.c`・`vkvideo-runtime.c:376`）、`mediafile/mediafile.c:52`（`open(O_CLOEXEC)`）・`:60`（fstat）・
  `:288`（`pread`、`off_t`）、`#if defined(__aarch64__)`（`vkvideo-runtime.c:43`、`vkvideo.c:24`）。

### 2.9 libtruetype と compat の library

| library | 行 | ISO C の外 | Windows |
| --- | --- | --- | --- |
| libtruetype | 4,208 | `companion.c:32-35`・`:250-284`（`open(O_RDONLY\|O_CLOEXEC)`・`fstat`・`read`） | mingw に `O_CLOEXEC` が無い（compile が通らない）。`O_BINARY` が無いと text の mode で読む。1 関数を直す（決定 6 の解釈、§6.3・問い U7） |
| libpng-compat | 950 | なし（`fopen("rb")`） | そのまま |
| libjpeg-compat | 3,890 | なし | そのまま。`jpeglib.h:66` の `typedef int boolean` は `windows.h` と同じ翻訳単位で衝突するので、`windows.h` を include する file（`os-win32.c` など）と分ける |
| libgif-compat | 1,150 | `decode.c:54`（`open` の file の口、`DGifOpenFileHandle(int)`） | compile は通る（mingw の io.h）。libbrowser は使わない（memory の口）。`O_BINARY` の欠けは file の口だけの問題として記録 |
| libz-compat | 2,200 | なし | そのまま（`uLong` は LLP64 で 32 bit、本物の zlib と同じ） |

- TrueType の collection（.ttc）は読める（`libtruetype/face.c:258-267`）。CFF の outline（`OTTO`）は読めない（`face.c:284`）。
- font の file（`userland/desktop/fonts/`）: Mahora（Zlib、zedBSD の物）、JetBrains Mono（OFL）、Droid Sans Fallback（Apache-2.0）。license の文が同じ directory にある。

### 2.10 host の道具（2026-11-01 の host、`which`・`dpkg`）

- 有る: `x86_64-w64-mingw32-gcc` 14（既定の thread の model は win32、`-posix` の版もある）、mingw-w64 12.0.0 の header と import library
  （`libucrt.a`・`libbcrypt.a`・`libcrypt32.a`・`libdwmapi.a`・`libimm32.a`・`libshcore.a`・`libuiautomationcore.a`・`libws2_32.a`・`libwinpthread.a`）、
  `x86_64-w64-mingw32-windres`、`llvm-rc-19`・`llvm-dlltool-19`、Debian の clang 19（X86・AArch64）、wine 10.0（`x86_64-windows/vulkan-1.dll`・`winevulkan.dll`）、
  libvulkan-dev 1.4.309、mesa の lavapipe、glslangValidator。
- 無い: `x86_64-w64-mingw32-clang`・`aarch64-w64-mingw32-*`（arm64 の Windows の cross）、`ld.lld`・`lld-link`、mingw の側の Vulkan の header と `vulkan-1` の
  import library。
- apt の候補にある（未導入）: `gcc-mingw-w64-ucrt64` 14.2（UCRT で build された mingw-w64 の gcc、`x86_64-w64-mingw32ucrt-gcc`）。
- gcc の spec は `-mcrtdll=ucrt` で `-D_UCRT` と `-lucrt` にする（`x86_64-w64-mingw32-gcc -dumpspecs` の 23 行・71 行）。既定は `-lmsvcrt`。
- 共有の toolchain（`toolchain/llvm/llvm.mk:353`・`:361`）は clang・lld で X86・AArch64 を含むが、mingw の runtime・header を持たない。

### 2.11 compiler と ABI の差（LLP64 ほか）

- Windows の `long` は 32 bit。libbrowser の `long` の使い方 62 か所を見た。挙動が変わりうるのは `css/values.c:7012-7013`（`(long)number` で整数かを
  判定、2^31 以上の値は Windows で未定義の動作）だけ。他（`net/url.c:91` の位置、`net/loader.c:168`・`net/net.h:90` の max-age、`page/input.c` の
  tabindex、`net/tls.c:80-98` の OpenSSL の型 = OpenSSL も `long`）は値の範囲が 2^31 に届かないか、相手の型と同じ。
- `printf` の `%zu` などは UCRT なら正しい。msvcrt では mingw の `__USE_MINGW_ANSI_STDIO` が要る → UCRT に決める（§6.1）。
- DLL の境: 全部の DLL と OpenSSL を同じ UCRT で build すれば heap は 1 つ（UCRT は process に 1 つ）。それでも公開の API は「DLL の外で確保した memory を DLL の中で放さない」
  形にする（他の CRT で build した program・試験の client が使っても壊れないため。今の `browser_view_dump` の `char **text` は §3.8 の `browser_free` で放す）。
  （今の公開の API は `browser_view_dump` の `char **text` を返す。§3.8 で解放の関数を足す）。

### 2.12 照合

Explore の補助の報告のうち、設計が頼る次の行を P7 が grep で照合した: `vkvideo-runtime.c:1361`、`engine.c:206`・`:216-217`、`present.c:65`、
`keiland_window.h:648-656`、`mediafile.c:52`・`:288`、`browser/shell/window.c:210`、`touch.c:369`、`mail.c:72-79`。他の libkeiland の行（window.h の範囲・
19 の file の数・74,200 行）は照合していない（p011 の最初の棚卸しで確かめる）。

## 3. engine の interface（決定 9・10）

### 3.1 形

```
 program (OmniGrammaton の shell・他の app・試験)
   │ browser.h（公開）
   ▼
 browser_instance ──登録── browser_storage_calls（ストレージバックエンド: memory・files・program の物）
   │ 1:n
 browser_view ─── page（統合）
   ├─ rendering engine: dom html xml css layout paint text image（+ vm の heap・string を土台に）
   ├─ script engine:    core = base vm js  ＋  access = bind（bind_host・bind_storage_calls で page に頼む）
   └─ net: url http tls cookie loader（cookie の jar・HTTP の cache は instance の物）
 すべての OS の呼び出し → base/os.h（private）→ os-posix.c ／ os-win32.c
```

- 決定 9 の「3 つはそれぞれ interface を持ち、その実装を instance に登録する」の読み（レビュー #18）: この設計で instance に**登録する**のはストレージバックエンドだけ。
  rendering と script は決定 10（今の tree で分かれている、その interface の分離を保てばよい）に従い、今の内部の interface のまま libbrowser の中で結ぶ（instance に
  差し替えの口を作らない。bind が dom・css の構造体を直に読む所も今のまま）。この読みでよいかは問い U15。
- rendering と script の分離は §2.4 の今の interface（`html_script_hook`・`bind_host`・`bind_storage_calls`・`js/js.h`）を保つ。WS334 は
  この interface の形を変えない（決定 10）。変えるのは storage が process の static から instance の物になる所（§3.5）だけ。
- 「各 engine を単体で使える」の意味（この設計での約束）:
  - script の core: base・vm・js（と OS の層）だけで library（静的な archive）になり、`js/tool.c`（`browser_script_tool` の中身）で script を走らせられる。§3.3。
  - rendering: 今も `browser_view` を script 無しの文書に使える（`--dump=layout` などの headless の mode）。rendering だけの別の library は作らない
    （DOM が vm の cell であり、決定 10 で DOM を出さないため）。単体の確かめは今の host の試験（`plan/ws074/tests/host-*.c` の layout・css・paint の物）。
  - storage: 公開の `browser_storage_calls` の実装（files）を libbrowser の外の program が直に呼べる。試験は表を直に呼ぶ host の試験（§7）。

### 3.2 script の core と access の境（今の形の確かめ）

- core（base 5,325・vm 24,089・js 45,162 行）が使う OS の物は O19〜O22 と `base/file.c`（`js/tool.c:384` の file の読み）だけ。これを §3.4 の OS の層に通すと、
  core は ISO C・`<math.h>`・`<setjmp.h>`・OS の層だけに依る。
- access（bind、55,359 行）は core と dom・css・html・net を使う。page が `bind_host` を満たす。
- 未確かめ: link の閉包（vm・js の .o が bind・dom の symbol を参照しないか）。include の graph では独立（§2.1）。p006 で archive を作り、
  `nm -u` で未定義の symbol が libc・OS の層・libm だけである事を確かめる。

### 3.3 script の core の library（node.js を参考に）

- node.js（Q1・P7 の知識。出典の取得はしていない）は、JavaScript の engine（V8）を library として持ち、embedder が V8 の platform の抽象（thread の task・時計）
  を与え、OS の event loop（libuv）と Node の binding を V8 の外に置く。V8 には d8 という単体の shell がある。
- 対応:

| node.js | libbrowser |
| --- | --- |
| V8（engine の library） | core = base・vm・js（`libbrowser-script.a`、新しい build の target） |
| V8 の platform の層（OS ごとの時計・乱数・時刻帯） | `base/os.h`（§3.4） |
| d8（単体の shell） | `js/tool.c`（`browser_script_tool`、試験の `--js` の mode） |
| Node の binding・libuv | bind（DOM の binding）・net の loader（thread と poll） |

- 決め: core の**公開の header は今は作らない**。core の interface は internal の `js/js.h`・`vm/vm.h` のまま。理由: 決定 9 が求めるのは「core だけで build して
  library にできる」事で、公開の安定な API は外の embedder が現れてからの判断（master-design-policy §2.6: 公開の header は大きな interface の決定の後）。問い U13。
- build: libbrowser の Makefile に「core の source の一覧」（`LIBBROWSER_SCRIPT_SOURCES`）を分けて書き、libbrowser.so はその一覧と残りから作る（source の重複は無い）。
  archive の target は試験と Windows の build で作る。

### 3.4 OS の層（`base/os.h`、libbrowser の private）

- 置き場: `libbrowser/base/os.h`・`os-posix.c`（zedBSD・Linux・FreeBSD）・`os-win32.c`。Makefile は OS で 1 つを選ぶ（`#ifdef _WIN32` の block を source に散らさない。
  guardrail の WS103 の「OS 固有は OS ごとの module に閉じる」と同じ考え）。
- 理由（なぜ libkeiland の `kl_thread`・`kl_socket` でないか）: WS327 の決定 2（libbrowser は単体の library なので、その wrapper を使わない）。libbrowser の OS の層は
  libkeiland の層と似た code になるが、master-design-policy §2.6（重複は欠陥でない、遅い抽象）に従い共有しない。
- 関数（名前は案。誤りは errno の値、Win32 の誤りは WS327 §5.3 と同じ表で写す）:

| 群 | 関数 | POSIX | Win32 |
| --- | --- | --- | --- |
| socket | `wb_os_socket_open`・`_connect`（非 blocking、EINPROGRESS）・`_connect_result`・`_send`（SIGPIPE を出さない）・`_receive`・`_close`、型 `wb_os_socket`（`intptr_t`、無効は -1） | socket・`MSG_NOSIGNAL` | `WSASocketW`（overlapped、継承しない）、`WSAStartup` を once で |
| 名前の解決 | `getaddrinfo` をそのまま使う（WinSock にもある）。`WSAStartup` は socket の群の once が先に済ませる | — | ws2tcpip |
| 待ち | `wb_os_poll(items, count, timeout_ms)`（READABLE・WRITABLE・FAILED） | poll | select（WS327 §5.2 の WSAPoll の不具合の理由。`fd_set` を数に合わせて確保） |
| 起こし | `wb_os_waker_open`・`_ring`・`_drain`・`_close`・`_socket`（poll の項に入る） | pipe2 | loopback の TCP の組（WS327 §5.2）。accept の後に相手の address と port が自分の connect の物かを確かめる（他の process の割り込みを断る） |
| thread | `wb_os_thread_name(name)` | Linux・FreeBSD は `pthread_setname_np`、zedBSD は付けない | `SetThreadDescription`（無ければ付けない） |
| 時刻 | `wb_os_monotonic_ns`、`wb_os_wall_ms`、`wb_os_local_offset_ms(time)`、`wb_os_zone_name(time, buffer)` | clock_gettime・localtime_r | QPC・`GetSystemTimePreciseAsFileTime`・`SystemTimeToTzSpecificLocalTimeEx`・`GetDynamicTimeZoneInformation`。Windows は略号（JST）でなく key の名前（"Tokyo Standard Time"）と地域の言葉の `StandardName` を出すので、`js/builtin_date.c:200-212` の略号の表は使えない。Windows の key → 英語の長い名前（JavaScript の Date の文字の形）の表を `os-win32.c` に持つ（CLDR の windowsZones を「Values referenced:」でデータとして引用、Unicode License。レビュー #13） |
| memory | `wb_os_aligned_alloc(alignment, size)`・`wb_os_aligned_free`（O30） | `posix_memalign`・`free` | `_aligned_malloc`・`_aligned_free` |
| 乱数 | `wb_os_random(bytes, length)` | `/dev/urandom`（今と同じ） | `BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)` |
| file | `wb_os_file_read_all`・`_write_all`（`base/file.c` の中身）、`wb_os_full_path`、`wb_os_current_directory`、`wb_os_make_directories`、`wb_os_replace`（一時の file を置き換える）、`wb_os_list`（directory の名前） | open・realpath・getcwd・mkdir・rename・readdir | UTF-8 → UTF-16 の `_wopen(O_BINARY)`・`GetFullPathNameW`・`_wgetcwd`・`CreateDirectoryW`・`MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)`・`FindFirstFileW` |
| path と URL | `wb_os_path_from_url(path_bytes)`・`wb_os_url_path(local)` | そのまま | `/C:/x` ↔ `C:\x`、UNC |
| library | `wb_os_library_open(names[])`・`_symbol`・`_close` | dlopen | `LoadLibraryExW(LOAD_LIBRARY_SEARCH_APPLICATION_DIR \| LOAD_LIBRARY_SEARCH_SYSTEM32)`（DLL の乗っ取りを避ける）。.exe の始めに `SetDefaultDllDirectories` を同じ flag で（依存の DLL の探索も狭める） |
| 場所 | `wb_os_data_directory`（既定の font） | `KEILAND_DATADIR`（今と同じ） | .exe の directory + `\share`（`GetModuleFileNameW`） |
| 根の証明書 | `wb_os_trust_roots(each, context)`（DER を 1 つずつ渡す） | 何もしない（OpenSSL の既定の path） | `CertOpenSystemStoreW("ROOT")`・`CertEnumCertificatesInStore`（§6.6） |
| 名前 | `wb_os_platform_name`（navigator.platform）、`wb_os_user_agent_os`（UA の括弧の中） | "zedBSD x86_64"・"X11; Linux x86_64"（今と同じ値） | "Win32"・"Windows NT 10.0; Win64; x64" |

- thread の基本（`pthread_create`・mutex・cond・key・once）は書き換えない。Windows では mingw-w64 の winpthreads を静的に link する（§6.1）。
  理由: libbrowser の変更を小さく保つ（ws.md の Resume point: libbrowser の修正の範囲は限られる）。MONOTONIC の cond（O5）が winpthreads で効かない時は
  `net/loader.c` の待ちを `wb_os` の待ちに変える（p004 の分岐、§12 R3）。
- 呼べる thread: どれも任意の thread から。waker は ring がどの thread からでも、drain は待つ thread だけ。

### 3.5 ストレージバックエンド（公開、新）

#### 3.5.1 何を受け持つか

| space | 中身 | key | 記録の形 | 書く時 |
| --- | --- | --- | --- | --- |
| `BROWSER_STORAGE_COOKIES` | 期限のある cookie（RFC 6265 §5.3 の persistent-flag が真の物）。session の cookie は memory だけ | domain・path・name を 16 進にした物（長ければ §3.5.3） | libbrowser の版付きの binary（name・value・domain・path・期限・作った時・最後に使った時・flags） | 保存・削除・期限切れの掃除の時 |
| `BROWSER_STORAGE_CACHE` | HTTP の cache の entry（今の `loader_cache_*` の条件と同じ: 200、max-age か ETag、no-store でない、plain な GET） | URL の 16 進（長ければ §3.5.3） | 版・URL・status・Content-Type・max-age・no-cache・ETag・保存の時・headers・body | entry を足す・除く時 |
| `BROWSER_STORAGE_LOCAL` | localStorage の origin ごとの領域 | origin の bytes の 16 進（今の file の名前と同じ） | 今の `.ls` の形（`STORAGE_MAGIC` + 長さと UTF-16）をそのまま | 変更のたび（今と同じ） |

- sessionStorage は保存しない（仕様の通り tab の寿命）。今は process に 1 組なので、instance の物にする（view ごとにはしない。挙動を変えない）。
- 「その他 local に保存しないと働かない物」: 今の libbrowser には上の 3 つしか無い（§2.5）。space の番号を足せる形にしておく（履歴・HSTS・許可は将来）。

#### 3.5.2 interface（案、`browser.h` に足す）

```
/* What a backend writes into: append bytes to the record being read. */
struct browser_storage_sink;
int browser_storage_sink_append(struct browser_storage_sink *sink, const void *bytes, size_t length);

struct browser_storage_calls {
	int version;                                   /* BROWSER_STORAGE_VERSION (1) */
	int (*read)(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
	int (*write)(void *context, unsigned space, const char *key, const void *bytes, size_t length);
	int (*remove)(void *context, unsigned space, const char *key);
	int (*list)(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
	int (*clear)(void *context, unsigned space);
	void (*close)(void *context);                  /* the instance is destroyed; may be NULL */
};
```

- **memory は libbrowser の外へ渡さない**: read は libbrowser の sink に書き足す（DLL ごとの heap の違いのため、§2.11）。key は libbrowser の文字列を借りる。
- 誤り: 0、ENOENT は使わない（無い時は found = 0）、ENOSPC（容量）、EIO（書けない）、ENOMEM。誤りの時 libbrowser は memory の内容で続け
  （今の「書けない file は memory に残る」`page/storage.c:20-21` と同じ）、view の console に 1 回だけ知らせる。
- 並行（レビュー #3 で直した）: backend の表を呼ぶのは instance の **storage の thread**（instance ごとに 1 本、名前 `browser-storage`）だけ。
  page の thread と network の thread は storage の thread の queue に書き（write・remove・clear）を積むだけで、disk を待たない（write-behind）。
  読み（instance を開く時の cookie の全部、cache の body、LOCAL の領域）は storage の thread に頼み、頼んだ thread は答えを待つ（cache の body の読みは network の thread が
  `loader->lock` を持たずに待つ。LOCAL の初めての読みは page の thread が待つ。今の同期の file の読みと同じ待ち）。backend は thread-safe でなくてよい。
  呼ばれた中で libbrowser の関数を呼ばない（例外は `browser_storage_sink_append` だけ）。
- lock の順（外 → 内）: `loader->lock` → instance の jar の lock（今の `cookie_lock` を instance へ）→ storage の queue の lock。storage の queue の lock が一番内側で、
  その中で他の lock を取らず、backend も libbrowser も呼ばない。
- 寿命: 表と context は instance の寿命の間借りる。`close` は instance の破棄の最後に 1 回。
- 再入: backend の呼び出しの中から instance・view を操作しない（browser.h の callback の約束と同じ型）。

#### 3.5.3 key の約束

- key は `[0-9a-f-]` の 1〜200 文字（Windows の名前の 1 部分の上限 255 の中、file の backend が file の名前にそのまま使える）。
- 元の key は長さの前置きの付いた field の並び（cookie は domain・path・name の 3 つ、cache は URL、LOCAL は origin）の bytes（区切りの無い連結は name が空の時に
  別の cookie と同じになるので使わない、レビュー #7）。その 16 進が 200 以下ならそれが key。超える時は 64 bit の hash（`storage/records.c` に新しく持つ FNV-1a 64。
  今の `base/hash.c` の `wb_hash_bytes` は 32 bit、`base/base.h:140`）の 16 進 + `-` + 衝突の番号（0 から）。
- どの記録も中に元の key を持つ。**書く前に**同じ key の記録を読み、元の key が違えば衝突の番号を 1 つ進めた key を試す（上書きで他の記録を消さない）。読みでも元の key を照合する。
- LOCAL の今の `.ls`（`STORAGE_MAGIC` "KEILAND LOCAL STORAGE 1"、origin を持たない、`page/storage.c:851-861`）は読める（v1）。書く時は origin を持つ v2 にする。
  今の file は origin の 16 進の名前のまま（長さの制限なし）なので、files の backend は LOCAL の読みで key が見つからない時に v1 の名前（origin の全部の 16 進）を探し、
  見つかれば v2 として新しい key で書き直す（移行）。

#### 3.5.4 libbrowser の中の持ち方

- instance が持つ: cookie の jar（今の static を instance の構造体へ、jar の lock 付き。上限 512 は今の値）、HTTP の cache の**永続の索引**（URL → 大きさ・鮮度・
  最後の使用。storage の thread だけが触る）、Web Storage の領域（今の static の vector を instance へ）、storage の thread と queue、backend の表。
- **memory の HTTP の cache は今のまま loader ごと**（`net/loader.c` の `loader_cache_*`、64 MiB、network の thread だけが触る。`request->validating`（`:1942`）などの
  生の pointer が今の通り安全）。永続の cache は「memory に無い時に storage の thread に body を頼む 2 段目」で、entry は copy で受け渡す（レビュー #3: 共有の memory の cache は
  view ごとの network の thread の間で解放と参照が競う）。
- 開く時: instance の作成で cookie の space を `list` → `read` で全部読み、期限切れを捨てる。cache の space は `list` で索引だけを作る（body は使う時に `read`）。
  LOCAL は今と同じく初めて使う時に読む。
- cookie の期限: jar の cookie に期限（UTC の ms）を足し、Expires・Max-Age を保つ（今は過去の時の削除だけ）。期限の過ぎた cookie は使う時と開く時に捨てる。
- cache の上限: backend の cache の space の合計を `BROWSER_INSTANCE_CACHE_LIMIT`（既定 256 MiB、instance の options で変えられる）で LRU にする。
  disk の cache を第 1 段で入れるかは問い U5。

#### 3.5.5 実装（backend）

| 実装 | 置き場 | 中身 |
| --- | --- | --- |
| なし（NULL） | — | 何も保存しない。今の localStorage の file も書かない。headless の試験の既定 |
| files | `libbrowser/storage/files.c`（新、libbrowser に入れ公開の関数 `browser_storage_files_open(directory, &calls, &context)` で渡す） | `<directory>/<space の名前>/<key>`。LOCAL は `<directory>/local-storage/<key>.ls`（今の path。v1 を読み v2 を書く、§3.5.3）。書きは一時の file → `fsync`（Win32 `FlushFileBuffers`）→ `wb_os_replace`（Win32 は `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)`、antivirus が掴む間の失敗は短く数回やり直す）。OS の呼び出しは §3.4 の層だけ |
| program の物 | program | 表を満たせば何でもよい（試験の fake など） |

- directory を決めるのは program（browser の UI。§4.5 の `kl_app_storage_directory`）: zedBSD・Linux は `$XDG_DATA_HOME/keiland/browser`（無ければ `~/.local/share/keiland/browser`。今の localStorage と同じ親）、
  Windows は `%LOCALAPPDATA%\OmniGrammaton`（`SHGetKnownFolderPath(FOLDERID_LocalAppData)`）。browser の UI は libkeiland の `kl_app_storage_directory`（§4.5）で受け取る。libbrowser は環境変数を読まなくなる（O18 が消える）。
- 2 つの process が同じ directory を使う時: 今の libbrowser にも同じ問題がある（localStorage の file は最後に書いた物が勝つ）。files の backend は開いている間
  directory の lock の file に **OS の lock**（POSIX `flock`、Win32 `LockFileEx`）を持つ（process が落ちれば OS が外すので、残った file で永久に memory だけになる事は無い、
  レビュー #9）。取れなければ ENOLCK を返して instance は memory だけで動く（console に知らせる）。Windows の 2 つ目の起動は、既にある窓に URL を渡す
  （§4.7 の単一の起動）ので普通は起きない。
- cookie は平文で保存される（今の zedBSD の localStorage も平文）。Windows の Chrome は DPAPI で暗号化する（知識）。問い U14。

### 3.6 状態・所有・寿命・並行性（instance と view）

- `browser_instance_create(options, &instance)` → `browser_view_create`（options に instance）→ … → view を全部 destroy → `browser_instance_destroy`。
  view が残る instance の destroy は EBUSY で断る（何もしない）。
- instance は作った thread の物。view は今と同じく作った thread で使う。instance の storage の lock は network の thread からも取られる。
- view の options の instance が NULL の時: libbrowser が process に 1 つの「既定の instance」（backend なし = 何も保存しない）を once で作って使う（process の終わりまで残す）。
  v3 との互換ではない（`view.c:321` は今も版の違う options を断る）。Keiland の UI（OmniGrammaton）は必ず files の backend の instance を作るので、zedBSD の localStorage の
  保存は続く（レビュー #8）。
- **1 つの instance とその view は 1 つの thread で使う**（Web Storage の領域は今と同じく page の thread だけが触るため。network の thread が触るのは jar と storage の queue だけ）。
  違う thread の view は別の instance を使う。
- `browser_instance_destroy` は int を返す（view が残れば EBUSY）。instance の作成が失敗した時も、渡された backend の `close` を 1 回呼ぶ（context の解放の持ち主を 1 つにする）。
- 解放: `browser_instance_destroy` は storage の thread の queue を流し切ってから thread を止め、backend の `close` を呼ぶ。
- 試験の約束: 2 つの instance は cookie・cache・storage を分ける（今は process で共有）。

### 3.7 失敗と回復

| 場合 | 振る舞い |
| --- | --- |
| backend の read が EIO・壊れた記録（版・長さ・元の key が合わない） | その記録を無い物とし、remove を試みる。console に 1 行 |
| backend の write が失敗 | memory の状態で続ける（今の localStorage と同じ）。次の変更でまた書く |
| 容量（ENOSPC） | cookie・cache は古い物から捨てて 1 回やり直す。LOCAL は今の `STORAGE_QUOTA_BYTES`（5 MiB）の判定が先にある |
| instance の作成で cookie の読みが失敗 | 空の jar で作る（作成は失敗させない）。ENOMEM だけは失敗 |
| OpenSSL の DLL が無い | 今と同じ EPROTONOSUPPORT（`net/tls.c:10-12`）。shell が「HTTPS の部品が無い」を出す |

### 3.8 公開の API の変更（`BROWSER_API_VERSION` 3 → 4）

| 変更 | 理由 |
| --- | --- |
| `struct browser_instance`、`browser_instance_create`・`_destroy`、`struct browser_instance_options { version; storage calls と context; cache の上限 }` | 決定 9（instance に登録） |
| `struct browser_view_options` に `struct browser_instance *instance` を足す | 同上 |
| `struct browser_storage_calls`・`browser_storage_sink_append`・`browser_storage_files_open`・`BROWSER_STORAGE_*` | §3.5 |
| `struct browser_fonts` に `bold`・`fallback_mono` を足す（NULL は OS の層の data の directory の既定） | O24（Windows に `/usr/share` は無い） |
| `browser_view_dump` が返す text を放す `browser_free(void *)` | DLL の heap（§2.11）。今は呼び手が `free` する |

- 版の約束（browser-component の規則 7）: `version` の違う options は今と同じく ENOTSUP で断る。利用側は 4 つ（`browser/main.c`・`browser/shell/`、
  `userland/tests/browser-probe/main.c`、`plan/tools/browser-component/client.c`、`plan/ws074/tests/host-*.c` の 4 本 host-view・host-form・host-sticky・wpt-ready-render）。
  WS334 の Phase で全部を v4 に直す（試験の file は WS074 の物なので Q1 と P8 の調整、§9）。
- `exports.map` に `browser_instance_*`・`browser_storage_*`・`browser_free` を足す。Windows の DLL の export は exports.map の glob を .def に展開して作る（§6.2）。

## 4. browser の UI・libkeiland の backend・`apps/`

### 4.1 形（案 A、推し。決定 13・14）

```
 userland/desktop/browser/        browser の UI（main.c の中身・shell/）。source は 1 つ。libkeiland だけを使う
   ▲ 同じ source を compile
 ├─ zedBSD・Linux: /bin/browser    (userland/desktop/browser/Makefile、今の package のまま)
 ├─ apps/browser-win32/            Windows の入口・resource・manifest・Makefile（make browser-win64・browser-winarm64）
 ├─ apps/browser-qt/   (将来)      Qt6 の backend で動かす時の入口・Makefile
 └─ apps/browser-gtk4/ (将来)      GTK4 の backend で動かす時の入口・Makefile
             │
             ▼
 libkeiland（公開の API は 1 つ）
   backend: Wayland（今の ui/ ほか）・Win32（libkeiland/win32/、新）・Qt6・GTK4（将来）
             │
 libbrowser（engine 3 つ・ストレージバックエンド files・OS の層）、libmedia、libtruetype・compat
```

- **UI は 1 つ**: browser の窓・titlebar の controls・入力の扱い・touch は今の `userland/desktop/browser/shell/` のまま、どの OS でも同じ source を使う。
  OS と toolkit の差は libkeiland の backend が吸収する（2026-11-01 のユーザーの見立ての要点: Windows の Chrome も Windows の標準の部品の見た目ではなく自前の
  見た目で、自前の font でよい。今のブラウザに menu の帯は無く、menu が要る所は libkeiland の Win32 の backend が HMENU に写せる。libkeiland が作るのは HWND 1 つ）。
- **`apps/browser-<backend>/` は入口と build だけ**（決定 13 の `apps/browser-win32/`、将来の `apps/browser-qt/`・`apps/browser-gtk4/`）:
  - 入口: その OS の `main`（Windows は `wWinMain`・`wmain` で UTF-16 → UTF-8）から browser の `browser_main(argc, argv)` を呼ぶ。
  - build: libbrowser・libmedia・libkeiland（その backend の file の組）と browser の UI を compile し、その OS の形に包む（Windows は .exe・DLL・zip、Qt・GTK4 は
    Linux・macOS の配布の形）。
  - resource: icon・版の情報・manifest（Windows）、desktop file（Linux）など。
  - 中身の code（窓・入力・描画）は持たない。持つと UI が 2 つになり、zedBSD と別れる。
- **zedBSD の build と別れない仕組み**:
  1. UI・libkeiland の共通の部分・libbrowser の source は 1 つで、`apps/` は compile するだけ。
  2. source の一覧を 1 か所に: libbrowser・libmedia・libtruetype・compat・libkeiland（共通の file と backend ごとの file）・browser の UI の source の一覧を各 directory の
     `sources.mk`（変数だけ）に移し、zedBSD の `Makefile`・Linux の `Makefile.linux`・`apps/browser-win32/Makefile`・WS074 の `plan/ws074/tests/list-sources.sh` が同じ
     `sources.mk` を読む（list-sources.sh は WS074 の物。1 行の直しを Q1・P8 と調整）。
  3. libkeiland の公開の API は backend に依らず 1 つ（`include/keiland/`）。backend に無い機能は ENOTSUP を返す約束にし、UI はそれで続く（今の mail が system の無い時に
     続く `mail.c:72-79` と同じ形）。
- **UI の source を `apps/` へ移すか**: 移さない（推し）。zedBSD の image の package の仕組み（`ZEDBSD_USERLAND_PACKAGE`、apps.conf の `/bin/browser`）に乗っているため。問い U2。

### 4.2 決定 15: libkeiland の Win32 の backend の上で今の UI のまま（案 A）。Win32 を直に使う版（案 B）は予備

- 2026-11-01 ユーザーから、ブラウザの UI はまず libkeiland の Win32 の backend の上で今の Keiland の UI のまま移植し、libkeiland で足りなければ後で Win32 を直に使う版を
  作ればよい（大きな実装ではない）、という趣旨の決定を受けた（ws.md の決定 15）。理由の要点: Windows の Chrome も Windows の標準の部品の見た目（Fluent）を使わない、
  font は自前でよい、今のブラウザに menu の帯は無く要る時は HMENU に写せる、libkeiland が作るのは HWND 1 つ。
- 用語: **chrome** = page の外側の部品（toolbar・location の欄・tab・menu・設定の画面）。**backend** = libkeiland の中で OS・toolkit に合わせて差し替える部分。
- この決定で: browser の UI は 1 つ（§4.1）。Windows の app らしさは libkeiland の Win32 の backend が Windows の口に写して保つ（§4.4 の 9 項目）。将来の Qt・GTK4 も
  libkeiland の backend で、`apps/browser-qt`・`apps/browser-gtk4` は入口と build だけ（§4.1・§4.7）。
- **予備の案 B**（Win32 を直に使う版。今は作らない）:
  - 形: `apps/browser-win32/` に Windows の標準の部品の chrome（button・EDIT の location の欄）を書き、libkeiland に依らない共通の shell（`apps/browser-shell/`: 引数と
    headless・instance と storage の場所・view の callback・navigation・題・location の文字・鍵の名前・wake・Vulkan の手順・CPU の道）を browser の UI から分けて使う。
    量の目安: 共通の shell の refactor 約 10 LW と Windows の層 約 12 LW。
  - 切り替えの印（どれかが p015 の実機の確かめ・p016 の後で残った時に、Q1 がユーザーに切り替えを諮る）:
    1. libkeiland の Win32 の backend で、Windows の app として要る口（§4.4 の N1〜N3・N6・N7）が、Wayland の backend と同じ公開の API の約束を変えずには満たせない
       （libkeiland の公開の header の約束の変更が要り、zedBSD の app に響く）。
    2. location の欄など libkeiland の text の欄で、日本語の IME（TSF を含む）の振る舞いが Windows の利用者の期待に届かず、直す量が案 B の量（約 22 LW）を超える見込み。
    3. libkeiland の共通に compile できる file が少なく（R9b）、Win32 の backend の量が見積もり（p011・p012 で 26 LW）の 2 倍を超える。
    4. ユーザーが Windows の標準の部品の見た目を求めた時。
  - 切り替えても失わない物: libbrowser の OS の層・ストレージバックエンド・libmedia・build（`make browser-win64`）・試験・鍵の表（T1）・CF_HTML（T2）・ICO（T3）。

### 4.3 libkeiland の backend の作り方: Win32 直か Qt6 か（決定 8）

| 項目 | Win32 直（`libkeiland/win32/`） | Qt6 の backend（`libkeiland/qt/`） |
| --- | --- | --- |
| window と message loop | `CreateWindowExW`・`MsgWaitForMultipleObjectsEx` で `kl_app_dispatch(timeout)`。`kl_app_post` は内部の queue と event（`PostMessageW` は queue が満ちると失敗する） | `QGuiApplication`・`QWindow`、`processEvents(timeout)`、`postEvent`。C の libkeiland から C++ の層を呼ぶ |
| 窓の枠・snap | `WM_NCCALCSIZE`・`WM_NCHITTEST`（最大化の button の `HTMAXBUTTON` で Windows 11 の snap の配置）を直に | frameless の QWindow の上で、結局 native の handle（`winId()`）で Win32 を呼ぶ |
| IME | TSF（`ITfThreadMgr`・`ITextStoreACP`、C の vtable）を直に | `QInputMethod`（Qt が TSF を包む。質は Qt の版に依る） |
| DPI | manifest の Per-Monitor V2、`WM_DPICHANGED` | Qt が扱う |
| 配布 | 追加の DLL 無し | Qt6Core・Gui（約 15 MB）・`qwindows.dll`、LGPLv3 の動的 link の約束 |
| build | mingw の C。x86_64・arm64 とも llvm-mingw | Qt6 を Windows の x86_64・arm64 に cross build（Debian に無い）、C++ |
| 他の OS | Windows だけ | macOS・X11 にも同じ backend（Keiland は Linux・FreeBSD では Wayland の backend を既に持つ） |
| 似た例（知識、出典の取得はしていない） | Chrome は自前の toolkit（Views）で描き、OS には窓・DWM・IME（TSF）・UIA・dialog を頼る | Ladybird は Qt6 の UI |

- **推し: Windows は Win32 直**（問い U1、確かめ）。libkeiland は部品を自分で描くので Qt は窓と入力の中継だけになり、Windows では DLL と license の約束が増えるだけ。
  Qt6・GTK4 の backend は macOS や X11 など Wayland の無い所に出す時の物（将来の `apps/browser-qt`・`apps/browser-gtk4`）。

### 4.4 libkeiland の Win32 の backend が受け持つ Windows の口（9 項目）

| # | 口 | 最初の最小の部分（決定 14） | 後（WS334 の後半） |
| --- | --- | --- | --- |
| N1 | HWND 1 つ、custom frame（`WM_NCCALCSIZE` で client を題の帯まで広げる、`DwmExtendFrameIntoClientArea` で影）、`WM_NCHITTEST`（題の帯の drag、縁の大きさの変更、最大化の button の `HTMAXBUTTON` で Windows 11 の snap の配置）、最小化・最大化・閉じるの button（libkeiland が描き、hover と押下を自分で） | ○ 全部（titlebar の controls を題の帯に置くのが Keiland の browser の形のため） | Mica（`DWMWA_SYSTEMBACKDROP_TYPE`）の背景 |
| N2 | TSF の IME（composition と候補の窓の位置） | IMM32 の口で composition（`WM_IME_COMPOSITION`）と候補の位置（`ImmSetCompositionWindow`・`ImmSetCandidateWindow`）。MS-IME・Google 日本語入力は IMM32 の互換で動く | TSF（`ITextStoreACP`）: 周りの文字（`kl_window_text_context`）・再変換・touch keyboard・絵文字の panel |
| N3 | Per-Monitor DPI V2 | ○ manifest、`WM_DPICHANGED` の矩形、libkeiland の部品と titlebar を倍率で描く（libkeiland の canvas の倍率は p012 で確かめる）。page の中身は §4.6 | — |
| N4 | `IFileDialog`（`IFileOpenDialog`・`IFileSaveDialog`）を `kl_file_chooser_open` に | —（browser は使わない。ENOTSUP） | ○ |
| N5 | HMENU（`kl_menu_*` を `CreatePopupMenu`・`TrackPopupMenuEx` に） | —（browser は menu を使わない） | ○ |
| N6 | clipboard と OLE の drag と drop | clipboard の copy（`CF_UNICODETEXT`・`CF_HTML`） | 貼り付け、OLE の `DoDragDrop`（画像を外へ、`kl_window_start_drag_icon`）と drop の受け |
| N7 | 精密 touchpad の scroll・pen・touch | `EnableMouseInPointer(TRUE)` と `WM_POINTER*`（mouse・touch・pen を 1 つの口で）、`WM_MOUSEWHEEL`・`WM_MOUSEHWHEEL` の細かい値（精密 touchpad は 120 未満の値で滑らかに来る）。touch の慣性は今の `kl_gesture`・`kl_scroller` | DirectManipulation による精密 touchpad の慣性と pinch、pen の圧力・傾き（`POINTER_PEN_INFO`） |
| N8 | 暗い mode と accent の色 | 暗い mode: system の設定（`AppsUseLightTheme`）を読み `DWMWA_USE_IMMERSIVE_DARK_MODE` と libkeiland の theme に | accent の色（`DwmGetColorizationColor` か WinRT の `UISettings`）を libkeiland の theme の accent に、設定の変更の通知（`WM_SETTINGCHANGE`） |
| N9 | UI Automation の accessibility | — | ○（libkeiland の部品の木を `IRawElementProviderSimple`・`Fragment` に。page の中身は libbrowser に accessibility の木が無く範囲の外） |

- 最小の部分の目安: browser が Windows で起動し、page を表示・操作でき、location の欄に日本語を入れられ、snap・DPI・暗い mode が Windows の app として働く所まで。

### 4.5 libkeiland の Win32 の最小の部分（関数の一覧）

- **形**: libkeiland の Wayland の実装（`ui/window.c` ほか 14 の file が `struct kl_window` の Wayland の field を直に使う、§2.7）に継ぎ目を入れる案は選ばない（§11 A5）。
  代わりに **同じ公開の関数を Win32 の別の file の組（`libkeiland/win32/`）で持つ**（master-design-policy §2.6: 同じ契約の独立した実装）。OS と Wayland に触れない
  file（gesture・scroller・motion・canvas・text・theme など）は両方で compile する。どれが共通にできるかは p011 の最初に「Wayland・POSIX の header を include しない」を
  checker（grep）で決めて表にする（未確かめ、§2.12）。将来の Qt・GTK4 の backend も同じ形（`libkeiland/qt/`・`libkeiland/gtk4/`）。
- **一覧**（browser の UI が呼ぶ物、§2.6。Windows で働かせる）:

| 群 | 関数 | Win32 の写し |
| --- | --- | --- |
| app | `kl_app_open`・`kl_app_window_create`（`KL_PRESENT_NONE` と CPU の予備）・`kl_app_dispatch`・`kl_app_take`・`kl_app_post`・`kl_app_close` | 窓の class、`MsgWaitForMultipleObjectsEx` と `PeekMessageW`、内部の queue と event。modal な大きさの変更の間は `WM_ENTERSIZEMOVE` で timer を立て、`WM_TIMER`・`WM_SIZE` で event を出す |
| window | `kl_window_size`・`kl_window_set_title`・`kl_window_close`、event（`KL_WINDOW_KEY`・pointer・wheel・touch・focus・大きさ・閉じ）（`window.c:228-291`） | N1・N7。鍵は scan code（lParam の bit 16-23）と拡張の bit（24）→ evdev の番号（表、§8 T1）、打つ文字を `ToUnicodeEx` で event の `text` に |
| text input | `kl_window_text_input`・`kl_window_text_cursor`・`kl_window_text_context`、event `KL_WINDOW_TEXT_COMMIT`・`PREEDIT`・`DELETE` | N2 の最初の段 |
| clipboard | `kl_window_copy` | N6 の最初の段（§8 T2 CF_HTML） |
| titlebar の controls | `kl_window_set_controls`・`kl_window_focus_control_mode`・`kl_window_set_action_state`・`kl_window_set_control_parts`・`kl_window_set_control_text`・`kl_window_set_control_value`、event `KL_WINDOW_ACTION`・`KL_WINDOW_CONTROL_DONE` | N1 の題の帯に、libkeiland の canvas と theme で controls（戻る・進む・再読み込み・location の欄）を描く。欄の編集は libkeiland の text の欄（IME は N2）。Keiland では compositor が描く物（`browser/shell/titlebar.c:8-20`）で、compositor の code は持ち込めないので libkeiland の側で作る |
| Vulkan | `kl_window_vulkan_surface`（`vkCreateWin32SurfaceKHR`）、新 `kl_window_vulkan_extensions`（下の header の追加） | — |
| CPU の表示 | browser の CPU の道（新）が使う present | DIB section → `BitBlt`（`WM_PAINT`） |
| touch | `kl_gesture_*`・`kl_scroller_*` | 共通の file（OS に触れない見込み、p011 で確かめ） |
| 場所 | 新 `kl_app_storage_directory`・`kl_data_directory`（下の header の追加） | `%LOCALAPPDATA%\<application>`（`SHGetKnownFolderPath(FOLDERID_LocalAppData)`）、.exe の directory の `share\` |
| system | `kl_app_system` | NULL を返す（mail の道は system 無しで続く） |
| drag | `kl_window_start_drag_icon`・`kl_window_drag_fill`・`kl_window_press_serial` | 最初は ENOTSUP（shell は失敗を無視する）。N6 の後の段 |
| 内部 | `platform/platform.h` の `keiui_*`（`keiui_thread_id` = `pthread_t`、`platform.h:21`・`:55`） | `platform-win32.c`（起こしと lock を Win32 で） |
| thread・socket | WS327 の `thread-win32.c`・`socket-win32.c` | そのまま link。F-WS327-win32-build の W5（libkeiland.dll を thread の生きている間に unload しない = 静的な依存で load）と S8（`-lws2_32 -liphlpapi`、`_WIN32_WINNT=0x0A00`）を Windows の Makefile の約束にする |

- **最小の部分の外の公開の関数**: compile と link は通し、Windows では ENOTSUP か NULL を返す stub（`libkeiland/win32/unsupported.c`、export の一覧から作る）。
  Wayland の型を返す関数（`kl_app_display` など、`keiland_app.h:159`）も NULL。header は前方宣言なので変えない。
- **公開の header の追加（KL_VERSION を 1 上げる、加えるだけ。Wayland の backend にも実装を足す）**:
  - `kl_window_vulkan_extensions(const char *const **names, uint32_t *count)`: 窓の Vulkan の instance に要る拡張（Wayland は `VK_KHR_surface`・`VK_KHR_wayland_surface`、
    Win32 は `VK_KHR_surface`・`VK_KHR_win32_surface`）。browser の UI が拡張の名前を持たなくて済む（`present.c:64-65` の直し）。
  - `kl_app_storage_directory(app, buffer, size)`（app の保存の場所。Wayland の backend は `$XDG_DATA_HOME/keiland/<application>`、無ければ `~/.local/share/keiland/<application>`。
    今の localStorage の親 `keiland/browser` と同じ）と `kl_data_directory(buffer, size)`（同梱の data。Wayland は `KEILAND_DATADIR`）。browser の UI に OS の path の code を置かないため。
  - 鍵の event の `text`（`struct kl_window_event` の既にある field、`keiland_window.h:159-186`）に打つ文字を入れる約束を足す（Wayland の backend は今のまま空でよい）。
  - `keiland_window.h:648-656` の comment を「`kl_window_vulkan_extensions` の拡張」に直す。

### 4.6 両方の案に共通の補い

- **高 DPI の page の中身**: libbrowser に device scale が無い（`devicePixelRatio` は 1 に固定、`bind/environment.c:1310`）。150% の画面で中身を CSS の 1 px = 物理の
  1 px で描くと文字が小さく、1/倍率で描いて GPU で拡大するとぼやける。engine の device scale は layout・paint・text に及び WS074 の範囲に触れる。問い U4（推し: 第 1 段は
  1 px のまま、engine の device scale は WS074 と調整した別の Phase）。
- **font**: 同梱の font（§6.8）。Windows の font を使う設定は問い U6。
- **描画の面**: Windows の Vulkan の loader（`vulkan-1.dll`）は GPU の driver が入れる。browser の UI が `kl_window_vulkan_extensions` で instance を作り、surface、FIFO の
  swapchain、`browser_view_record`。loader・device が無い時は CPU の道（`browser_view_draw_pixels` → CPU の表示）。`--no-gpu` の引数で CPU の道を選べる（試験用。zedBSD でも
  使える）。Direct3D 12 は使わない（WS234 の記述の D3D12 とは別。WS234 の再開の時に決める）。
- **accessibility**: libbrowser に accessibility の木が無い（aria の属性を DOM に持つだけ）。page の中身の UIA は範囲の外（Future Work の候補「libbrowser の accessibility の木と
  UIA・AT-SPI の provider」、Q1 に登録を頼む）。chrome の UIA は N9。
- `windows.h` を include するのは `apps/browser-win32/`・`libkeiland/win32/`・`libkeiland/platform/*-win32.c`・`libbrowser/base/os-win32.c`・`libmedia/media-os-win32.c` だけ
  （`jpeglib.h` の `boolean` の衝突を避ける、§2.9）。

### 4.7 `apps/browser-win32/` の中（案 A）

| file（案） | 中身 |
| --- | --- |
| `Makefile` | §6.9。libbrowser・libmedia・libkeiland（共通の file と `win32/`）と browser の UI を compile |
| `main.c` | `wWinMain`（`OmniGrammaton.exe`、GUI）と `wmain`（`omnigrammaton-cli.exe`、console。標準の出力を binary の mode に、O29）。UTF-16 の argv → UTF-8 → `browser_main` |
| `single.c` | 単一の起動（名前付きの mutex と `WM_COPYDATA` で既にある窓に URL を渡す） |
| `resources/OmniGrammaton.rc`・`OmniGrammaton.manifest`・`OmniGrammaton.ico` | §6.4 |
| `tools/make-ico.c` | host の道具: 今の browser の icon の絵を各大きさに描き ICO に書く（§8 T3）。host の cc で build |

- 将来の `apps/browser-qt/`・`apps/browser-gtk4/` も同じ形（`Makefile`・入口・その OS の resource）。

### 4.8 browser の UI（`userland/desktop/browser/`）の変更（案 A）

| file | 変更 |
| --- | --- |
| `main.c` | `main` の中身を `browser_main(argc, argv)` に（zedBSD の `main` はそれを呼ぶだけ。Windows の入口も呼ぶ） |
| `shell/present.c` | 拡張の名前を `kl_window_vulkan_extensions` から（:64-65）。`pApplicationName` を製品の名前に（:68）。Vulkan が無い時の CPU の道への切り替え |
| `shell/present-cpu.c`（新） | CPU の道（zedBSD は `KL_PRESENT_SHM`、Windows は libkeiland の DIB） |
| `shell/keys.c` | event の `text` があればそれを DOM の `key` に（JIS 配列で記号が正しく出る）。無ければ今の US の表 |
| `shell/window.c`・`shell/touch.c` | `clock_gettime`（:210・:369）を kl の時計（`keiland_thread.h`）へ。application の名前を build の定数に（§6.5） |
| `shell/shell.c`・`main.c` | instance と files の backend（場所は `kl_app_storage_directory`）、font の既定（`kl_data_directory`）（p005） |
| `shell/mail.c`・`shell/titlebar.c` | 変更なし |
| `Makefile`・`sources.mk`（新） | 一覧を `sources.mk` へ |

- zedBSD の挙動を変えない（guardrail の「refactor は build と最後の boot test」）。WS074 の guest の browser の試験で確かめる（§7.1 段 5）。
- WS074 の host の build（`plan/ws074/tests/host-build.sh`）は `browser/main.c` を `plan/ws074/tests/host-shell.c`（`shell_run` の代わり）と `-I$app` で compile する。
  `browser_main` への分けはこの 2 つに響くので、p009 で一緒に直す（WS074 の file。Q1・P8 と調整、§9.2）。

### 4.9 libkeiland の全体の移植の置き場（決定 14 の「後でじっくり」）

- WS334 の後半（p016）: browser の質に効く Windows の口（N2 の TSF、N6 の drag と drop、N7 の DirectManipulation と pen、N8 の accent、N1 の Mica、N9 の UIA の chrome の部分）。
- WS234（libkeiland の DLL 版、保留中）: browser が使わない libkeiland の API の全部の Win32 化（N4 の file の dialog、N5 の menu、設定・system・media の選び・glass・desktop など）と、
  Windows の上で Keiland の他の app（Files・Terminal など）が動く事。WS234 の「同じ .exe が zedBSD と Windows で動く」目標はこの上に立つ。WS234 は 2026-10-14 にユーザーが
  保留にしたので、再開の時期はユーザーが決める。
- この分け方は WS の目標から決まる（AGENTS.md §2: WS は一つの具体的な到達目標。WS334 は OmniGrammaton の .exe）ので、問い U1 では確かめだけにする。
- WS334 から WS234 へ渡す物: `libkeiland/win32/` の最小の部分、checker の表（共通に compile できる file）、§4.3 の比較、WS234 の D3D12 の記述と Vulkan の食い違い。

## 5. libmedia（決定 7）

| # | 所 | 直し |
| --- | --- | --- |
| M1 | `engine.c:206-219`・`:259`・`:1018`・`:492-494`（pipe の起こし）と公開の `media_engine_wake_fd`、`media.h:22-26`（「engine は呼び返さない」の約束） | 起こしを callback にする（レビュー #4 で直した）: 起こしの関数と context は **`media_engine_open` の引数**で渡す（open の中で engine の thread が始まる `engine.c:221` ので、後から set すると最初の起こしを落とす）。callback は engine の thread から呼ばれ、呼び手は短く返す（libbrowser は lock を取らず印を立てるだけ）。`media.h` の約束の comment を書き直す。libbrowser の `page/media.c` の callback は新しい `net_loader_ring(loader)`（`loader->rang` を立てて network の thread を起こす。今の watch の fd が鳴った時の `loader.c:1122-1127` と同じ結果）を呼ぶ。`net_loader_watch`・`_unwatch`（`net/loader.c:736`）は使われなくなる（消すかは P8 と調整）。media の close は loader の destroy の前（今の順）。`media_engine_wake_fd` は POSIX の build に残すか消すか: 今の利用者は libbrowser だけなので消す（Q1 の確認） |
| M2 | `vkvideo-runtime.c:24`・`:1361-1363`・`:1379`・`:1433` | `dlopen` → OS ごとの小さな関数（POSIX は今のまま、Win32 は `LoadLibraryExW(L"vulkan-1.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)`・`GetProcAddress`）。libmedia の中の `media-os.h`（private、2 関数） |
| M3 | `vkvideo-runtime.c:1879-1881` の `VK_KHR_external_memory_fd` | GPU の絵を出す時だけ有効にする今の条件のまま。libbrowser は GPU の絵を使わないので Windows では要らない。Windows で GPU の絵を出すなら `VK_KHR_external_memory_win32`（将来） |
| M4 | `mediafile/mediafile.c:52`・`:60`・`:288` | `open(O_CLOEXEC)`・`fstat`・`pread` → OS ごと（Win32 は UTF-16 の `CreateFileW` と `ReadFile` に `OVERLAPPED` の offset、64 bit の大きさ）。`media-os.h` に入れる |
| M5 | pthread（99 か所） | winpthreads のまま（libbrowser と同じ判断） |
| M6 | `engine.c:794-802`・`:1029`・`:1043`（clock・timedwait・nanosleep） | winpthreads にある（`pthread_time.h`）。そのまま |
| M7 | `#if defined(__aarch64__)`（`vkvideo-runtime.c:36-46`、`vkvideo.c:18-28`） | **CPU の分岐でなく Raspberry Pi 4 の firmware の decoder の回避**（comment にそう書いてある。失敗した decode を黙って飛ばし、4 枚を飛ばしたままにする）。arm64 の Windows で効くと誤る。回避の条件を CPU から device（`VkPhysicalDeviceProperties` の vendor・device の ID か driver の ID）に替える（レビュー #6）。`vkvideo.c` も変える file に入れる |
| M9 | `picture.c:442`（GPU の絵の fd の `close`） | GPU の絵を出す時だけの道。Windows では作らない（M3）ので、`close` を POSIX の build だけの file へ移すか `media-os` の関数に |
| M8 | Vulkan Video の Windows の driver | NVIDIA・AMD・Intel の Windows の driver は `VK_KHR_video_decode_h264` を出す（知識、未確かめ）。出さない driver・Wine では software の H.264 に落ちる（今の順、`decoder.c:97-101`） |

- Vulkan の側（決定 7 の「Vulkan と Vulkan Video の実装の側も直してよい」）: Windows では Vulkan の実装は GPU の vendor の driver で、zedBSD の物ではない。
  直せるのは zedBSD の libvulkan・driver で、Windows の port には関わらない。Windows の driver の不具合は libmedia の側で避ける（記録だけ）。

## 6. build

### 6.1 cross compile の toolchain（案。導入は Q1 の許可、問い U3）

| 案 | 中身 | x86_64 | arm64 | 今の host |
| --- | --- | --- | --- | --- |
| T-A（推し・本線） | llvm-mingw（clang・lld・mingw-w64 の runtime・UCRT。`x86_64-w64-mingw32-clang`・`aarch64-w64-mingw32-clang`）。版を固定した release の tarball を取得・SHA-256 を照合して `build/` の外の共有の場所へ（`build/sources` と同じ扱い）。Windows の host でも同じ toolchain（Windows 版の release）が使える | ○ | ○ | 無い（導入が要る） |
| T-B（先に使う） | Debian の mingw-w64 gcc 14 の **UCRT 版**（package `gcc-mingw-w64-ucrt64` 14.2、`x86_64-w64-mingw32ucrt-gcc`。今の host には無く、apt の候補にある）。今入っている msvcrt の版（`x86_64-w64-mingw32-gcc`）に `-mcrtdll=ucrt` を付ける形は、libgcc・libmingwex が msvcrt で build されていて混ざるので使わない（レビュー #11） | ○ | × | 導入が要る（host の package） |
| T-C | 共有の LLVM（`toolchain/llvm/`）に Windows の target の runtime を足す | ○ | ○ | toolchain の変更（LLVM の作り直し・cache）。選ばない |
| T-D | MSVC（clang-cl・link.exe） | ○ | ○ | 不可: libbrowser の GC が `__builtin_unwind_init` 等の gcc・clang の拡張に頼る（O26）。Windows の SDK の license も host の外 |

- 推し: Q1 の許可の後、p002 で T-A（llvm-mingw）を入れて x86_64 と arm64 の両方を build する。T-A の許可が遅れる時は、T-B（Debian の UCRT 版の gcc。apt で入る）で x86_64 だけを先に立てる。
  以後の本線は T-A（決定 11: clang の mingw を好む、arm64 の Windows）。T-B は warning の第 2 の compiler として残す。
- CRT は UCRT（Windows 10 以降は OS の一部。`%zu` などが正しい、UTF-8 の active code page が効く）。OpenSSL も同じ toolchain・同じ UCRT で build する（`errno`・heap を揃える）。
  thread は winpthreads を 1 つの `libwinpthread-1.dll` として同梱する（libbrowser.dll と libmedia.dll の 2 つに静的に入れる形は、thread ごとの片付けが 2 組になるので避ける。
  動けば静的でもよいが、確かめが要る）。libgcc は gcc が必ず link するので `-static-libgcc`（llvm-mingw は compiler-rt）。C++ の runtime は使わない。
  llvm-mingw の libwinpthread の静的な版）。libgcc・libc++ は使わない（C だけ）。
- flags: `-std=gnu11`（libbrowser の host の build と同じ）`-Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE`
  （Win32 の W の API だけを使う。A の API は使わない）。
- 対象の版: **Windows 10 1903 以降**の x64 と Windows 11 の arm64（manifest の `activeCodePage` UTF-8 が 1903 から。libtruetype の `companion.c:250`・`browser/main.c:590`・
  `net/http.c:74`・`js/builtin_global.c:110` の narrow の path の open が、日本語の利用者名の folder でも UTF-8 で開けるため。レビュー #12）。`SetThreadDescription` の有無は実行時に探す（WS327 W6）。

### 6.2 成果物

```
build/browser-win64/                 (arm64 は build/browser-winarm64/)
  obj/ ...                           中間の物（object・.def・import library・resource の .o・toolchain.txt）
  OmniGrammaton/                     配る形（zip の中身、§6.7）
    OmniGrammaton.exe                GUI の subsystem（-mwindows）。browser の UI（userland/desktop/browser）+ apps/browser-win32 の入口
    omnigrammaton-cli.exe            console の subsystem。同じ browser の UI の headless の mode（試験・script 用）
    libbrowser.dll  libmedia.dll  libkeiland.dll
    libssl-3-x64.dll  libcrypto-3-x64.dll   (arm64 は libssl-3-arm64.dll・libcrypto-3-arm64.dll)
    share/fonts/*.ttf  share/browser/{start,about,text}.html
    share/licenses/...               (§6.8)
  OmniGrammaton-<版>-win64.zip       (browser-win64-zip)
  tests/                             試験の .exe（host-*.exe・os-test.exe・storage-test.exe、§7）
```

- DLL にする物: libbrowser・libmedia（公開の API の境。決定 9 の engine の単体性）。libtruetype・compat の library（png・jpeg・gif・z）は libbrowser.dll に
  静的に link（libmedia は compat を使わない）。`libkeiland.dll`（共通の file と Win32 の backend）も DLL（F-WS327 の W5、WS234 の DLL 版の土台）。
- export: libbrowser は `exports.map` の glob を objects の `nm` で展開した .def（build の規則の中）。libmedia は `libmedia/exports.map` から。
  `__declspec(dllexport)` を公開の header に足さない（header を変えない）。
- import library と Vulkan の loader（レビュー #2 で直した）: libbrowser（`paint/vulkan.c`・`view/view.c`）・libkeiland の Vulkan の present・browser の UI の `present.c` は
  `vk*` を prototype で直に呼ぶ。普通の import library で link すると、`vulkan-1.dll` の無い機械（GPU の driver の無い VM・RDP の一部・一部の arm64 の機械）で .exe が
  起動しない（CPU の道も `omnigrammaton-cli.exe` も使えない）。**遅延の import**（delay-load）にする: `vulkan-1.dll` の .def（Khronos の loader の export）から
  binutils の `dlltool --output-delaylib` で遅延の import library を作り link する。最初の `vk*` の前に、libkeiland（`kl_window_vulkan_extensions` が拡張を返さない）と
  libbrowser（`wb_os_library_open` で `vulkan-1.dll` を確かめる）が loader の有無を確かめ、無ければ Vulkan を呼ばずに CPU の道へ。llvm-mingw（lld）で遅延の import と
  mingw の `__delayLoadHelper2` が働くかは未確かめ（p002 で確かめる）。働かない時の予備は Windows だけの thunk の file（使う `vk*` 約 40 個を `vkGetInstanceProcAddr` で
  引いて呼ぶ、`paint/vulkan-win32.c` など）。受け入れに「`vulkan-1.dll` の無い所で `omnigrammaton-cli.exe --render` と `OmniGrammaton.exe --no-gpu` が動く」を入れる。
  OpenSSL は libbrowser が実行時に開くので import library は要らない。
  開くので import library は要らない。

### 6.3 libtruetype と compat（決定 6）

- 「そのまま使う」を「zedBSD の library を Windows の build にも使う（FreeType・本物の libpng などに替えない）」と解釈する。Windows で compile が通らない・壊れる
  所だけを最小に直す: libtruetype の `companion.c:250` の 1 関数（`O_CLOEXEC` が無い時は 0、`O_BINARY` を付ける。path は UTF-8 のまま: manifest の UTF-8 の
  active code page が効く Windows 10 1903 以降を対象にする、§6.1）。png・jpeg・z は変えない。gif は file の口の `O_BINARY` を足す（使わないが、壊れた口を残さない）。問い U7 で確かめる。
- warning: WS074 の host の build は compat と libtruetype を `-Wno-error` で compile している（`plan/ws074/tests/host-build.sh:58`・`:71`）。Windows の build も compat と
  libtruetype は `-Werror` を付けず、warning の数を `cross-build.sh` が数えて報告する（決定 6 の「そのまま」を守り、warning を消すための source の直しをしない）。
  「warning 0」の受け入れは WS334 が書く・変える source（libbrowser・libmedia・libkeiland・browser の UI・apps/）に当てる。問い U7。

### 6.4 resource と名前（OmniGrammaton）

- `.rc`: `VERSIONINFO`（`ProductName` OmniGrammaton、`FileDescription` "OmniGrammaton Web Browser"、`CompanyName` は問い U9、`LegalCopyright`
  "(C) 2026 Awe Morris"、`ProductVersion` は zedBSD の版の番号の決め方に合わせる = Q1）、icon（`.ico`、16・20・24・32・40・48・64・256 px）、manifest。
- manifest: `dpiAwareness` PerMonitorV2、`activeCodePage` UTF-8（Windows 10 1903 以降。A の API と CRT の narrow の関数が UTF-8 になる。保険で、code は W の API を使う）、
  `supportedOS` Windows 10・11、`longPathAware`、Common Controls は要らない。
- icon: 今の browser の icon は compositor の code で描く（`wayland/icons.c`）。推し: その絵を host の道具で各大きさに描いて .ico にする（ICO の形の writer、§8 T3）。
  新しい図案にするかは問い U9。
- 窓の題: 「page の題 - OmniGrammaton」（Chrome・Edge の慣例）。窓の class・Vulkan の application の名前・storage の directory も OmniGrammaton。
- zedBSD の上の名前との関係: 今の zedBSD の app は `/bin/browser`、apps.conf の「Browser」（`wayland/apps.conf:21`）。zedBSD でも OmniGrammaton と呼ぶかは問い U9
  （推し: 製品の名前は両方で OmniGrammaton、zedBSD の command の名前 `/bin/browser` は変えない）。

### 6.5 2 つの名前の定数

- shell の build の定数 `SHELL_PRODUCT_NAME`（"OmniGrammaton"）と `SHELL_APPLICATION_ID`（zedBSD は "browser" のまま: compositor の apps.conf・mail の名前
  `mail.c:48` が "browser" に頼る）。名前の変更で zedBSD の挙動を変えない。

### 6.6 TLS と証明書

| 案 | 中身 | 推し |
| --- | --- | --- |
| S-A | OpenSSL 3.5.8（`userland/packages/security/openssl/Makefile` と同じ版・SHA-256）を同じ toolchain で cross build（`./Configure mingw64`。arm64 の target 名と DLL の名前は未確かめ）し DLL を同梱。根は Windows の ROOT の store の写しと同梱の Mozilla の根（S-B）を合わせて `X509_STORE` に入れ、Windows の `Disallowed` の store の証明書は拒む（`wb_os_trust_roots`、`SSL_CTX_get_cert_store`・`d2i_X509`・`X509_STORE_add_cert`・`X509_free` を `net/tls.c` の symbol の表に）。**`SSL_CTX_set_default_verify_paths` は Windows では呼ばない**、OpenSSL は `--openssldir` を利用者が書けない path にし、設定の file を自動で読まない（`OPENSSL_INIT_NO_LOAD_CONFIG`。利用者が作れる path の `openssl.cnf` を読む類の弱さを避ける、レビュー #10） | ○（第 1 段） |
| S-B | S-A の根を Mozilla の `cacert.pem`（`ca-certificates` の package と同じ、MPL-2.0）にして同梱 | 予備（Windows の store が読めない時に `--ca-file` で） |
| S-C | SChannel（Windows の TLS、SSPI）で `net_tls_*`（`net/net.h:176-188`）の別の実装 `net/tls-schannel.c` | 将来の案。DLL が要らず、OS の失効の確かめ・根の更新が効く。code が大きく（`InitializeSecurityContextW` の loop）、zedBSD と TLS の挙動が分かれる |

- 推し S-A（問い U10）。理由: zedBSD と同じ TLS の code（`net/tls.c`）で、libbrowser の変更が library の名前と根の読み込みだけ。TLS は当面 OpenSSL という WS074 の
  決め（master.md の WS074 の行）とも合う。
- 既知の限り（S-A）: Windows は足りない根を必要な時に Windows Update から取る（ROOT の store は全部を持たない）ので、store の写しだけでは正しい site が失敗しうる。
  同梱の Mozilla の根を合わせて補う。Windows の検証（`CertGetCertificateChain`）を OpenSSL の検証の callback から呼ぶ形は第 2 段の案（失効・企業の方針も効く）。
- OpenSSL の build は外部 package の規則（tarball を取得・照合・patch、`userland/packages/` の境界、license は Apache-2.0）。置き場は
  `userland/packages/security/openssl/Makefile.windows`（新）か WS334 の build の script（p013 で決める）。

### 6.7 配布

- 第 1 段は zip（`make browser-win64-zip` の出力 `OmniGrammaton-<版>-win64.zip`、展開して .exe を開く、利用者の権限だけ）。installer（MSIX・WiX）と code の署名（無いと SmartScreen が警告する）は問い U11（推し: zip、署名は後）。
- 保存の場所（§3.5.5）は `%LOCALAPPDATA%\OmniGrammaton`。zip の中に書かない。

### 6.8 license の同梱

- `share\licenses\`: zedBSD（Zlib）、font 3 つ（Mahora は Zlib、JetBrains Mono は OFL-1.1、Droid Sans Fallback は Apache-2.0、`userland/desktop/fonts/`）、
  libbrowser の表（`userland/base/licenses/browser/WHATWG-entities`・`Unicode-License-V3`、`libbrowser/Makefile` の IMAGE_DATA）、OpenSSL（Apache-2.0）、
  mingw-w64 の runtime（静的に入る CRT の start-up・`libwinpthread-1.dll` の許諾の文。版で文が違うので p014 で取得した tarball の COPYING から写す）、
  Mozilla の根（S-B を使う時、MPL-2.0。今の `ca-certificates/MPL-2.0.txt`）、Vulkan-Headers は build の時だけ（配らない）。

### 6.9 `make browser-win64`・`make browser-winarm64`（決定 12・13）

- **target の名前**: `browser-win64`（x86_64）と `browser-winarm64`（arm64）。Windows の慣例の名前（win64・arm64）に合わせた。他に `browser-win64-zip`・
  `browser-winarm64-zip`（配る zip）、`browser-win64-tests`（試験の .exe）、`browser-win64-clean`（出力の directory を make の規則で消す。AGENTS.md §4 の例外:
  自分の build の出力を make の規則が消す）。
- **置き場**: 中身は `apps/browser-win32/Makefile`（単独で走る: `make -f apps/browser-win32/Makefile WIN32_ARCH=x86_64`。これが本来の入口で、Windows の host でも
  これを使う。一番上の Makefile は llvm.mk・全部の package の Makefile・`$(shell sed|grep|git …)` を読むので Windows の上では壊れやすい、レビュー #15）。一番上の `Makefile` には
  それを呼ぶだけの短い規則を足す（`keiland-linux` の形、`Makefile:825-834` と同じ。決定 12 の `make browser-win64`）。両方の入口を `cross-build.sh` で試す。
  ```
  browser-win64:   $(MAKE) -f apps/browser-win32/Makefile WIN32_ARCH=x86_64  BROWSER_WIN32_BUILD=build/browser-win64
  browser-winarm64: $(MAKE) -f apps/browser-win32/Makefile WIN32_ARCH=aarch64 BROWSER_WIN32_BUILD=build/browser-winarm64
  ```
  - 一番上の Makefile の作業の directory（tree の一番上）で走る。path は tree の一番上から（`userland/desktop/libbrowser/...`）。
  - config.mk が無くても走るよう、`ZEDBSD_CONFIG_OPTIONAL_GOALS` に足す（`Makefile:73-79` の config の確かめを避ける）。zedBSD の `$(BUILD)`（`build/<platform>`）
    とは別の出力にする。理由: Windows の build は zedBSD の platform の選びに依らず、`make browser-win64` を config 無しの clone でも走らせたいため。
    出力の場所は `BROWSER_WIN32_BUILD=` で変えられる（担当の使い捨ての出力 `build/<phase>/` に）。
- **toolchain の探し方**（`apps/browser-win32/Makefile` の中、上から順に最初に見つかった物）:
  1. `WIN32_CC=` を明示（例 `WIN32_CC=/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang`）。
  2. `LLVM_MINGW=<llvm-mingw の root>` があれば `$(LLVM_MINGW)/bin/$(WIN32_TRIPLE)-clang`（triple は `x86_64-w64-mingw32`・`aarch64-w64-mingw32`）。
  3. PATH の `$(WIN32_TRIPLE)-clang`。
  4. x86_64 だけ: PATH の `x86_64-w64-mingw32ucrt-gcc`（Debian の UCRT 版の mingw-w64）。msvcrt の版の `x86_64-w64-mingw32-gcc` は使わない（§6.1）。
  5. どれも無ければ make を止め、要る物（llvm-mingw の入手の案内と `LLVM_MINGW=` の例）を 1 行で出す。
  - 同じ探し方で `WIN32_WINDRES`（`<triple>-windres`、無ければ `llvm-windres`・`llvm-rc`）、`WIN32_DLLTOOL`、`WIN32_AR`、`WIN32_NM`、`WIN32_OBJDUMP` を決める。
  - 使った compiler の名前と版を `build/browser-win64/obj/toolchain.txt` に書く（試験の記録の再現のため）。
- **Makefile の中身**: libbrowser・libmedia・libtruetype・compat・libkeiland・browser の UI の source の一覧は各 directory の `sources.mk` を include して読む（§4.1 の 2。zedBSD の
  Makefile と同じ一覧）。Windows だけの source（`os-win32.c`・`media-os-win32.c`・`libkeiland/win32/*.c`・`libkeiland/platform/*-win32.c`・`apps/browser-win32/*.c`）を足し、
  posix の物を外す。browser の headless の `--render` の PNG の書き出し（`userland/desktop/picture/png-write.c`、`browser/Makefile:17`）も入れる。object は
  `$(BROWSER_WIN32_BUILD)/obj/<tree の path>.o`。
- **compat の header**: `include/libc/compat/` は zedBSD の libc の header（`stdio.h` など）の隣にあるので `-Iinclude/libc` にしない（mingw の header が隠れる）。
  `keiland-linux.mk:26-33` と同じく `include/libc/compat/` を `$(BROWSER_WIN32_BUILD)/include/compat/` に**写して**使う（symlink にしない: Windows の host で困る）。
- **clean**: `browser-win64-clean` は `BROWSER_WIN32_BUILD` が空・`/`・`build` の時に止まる確かめを先に置く。
- **Windows の host で**: 同じ Makefile を Windows の上の GNU make と llvm-mingw の Windows 版で走らせる（`make browser-win64 LLVM_MINGW=C:/llvm-mingw`）。
  recipe は `sh` を前提にする（Git for Windows か MSYS2 の `sh`。cmd を前提にしない）。§7.4。
- **Vulkan の header と import library**: Vulkan-Headers を版を固定した tarball で取得（SHA-256 を照合）して `$(BROWSER_WIN32_BUILD)/sources/` に置き（担当の worktree から共有の
  `build/sources/` に書かない、AGENTS.md §7。共有の写しが要れば Q1 が置く）、`$(BROWSER_WIN32_BUILD)/obj/include/vulkan/` に写す。import library は §6.2（遅延の import）。
  取得の規則は `make -f apps/browser-win32/Makefile fetch`（`make download` は使わない）。
- **Wine での試し（任意）**: `make browser-win64-wine-smoke`（`omnigrammaton-cli.exe --js` と `--dump=layout` の数件）。判定には使わない（決定 11）。

## 7. 試験

### 7.1 段階（決定 11）

| 段 | 何で | 何を | 判定 |
| --- | --- | --- | --- |
| 1 | Linux の host | OS の層の posix の側への置き換えで既存の挙動が変わらない: `plan/ws074/tests/host-build.sh` の plain・asan、host-*.c（91 本）、`run-js-tests.py`・`run-test262-parse.py` の件数が置き換えの前と同じ、`plan/tools/browser-component/run.sh plain asan` | 件数の一致（前後を数えて比べる） |
| 2 | Linux の host の cross build | `make browser-win64`（T-B、許可の後は T-A）と `make browser-winarm64`（T-A）で全部の library・.exe を build、warning 0。`nm` で DLL の export が exports.map の展開と一致、`objdump -p` で依存の DLL が §6.2 の一覧だけ | warning 0、一覧の一致 |
| 3（任意） | Wine 10 | `omnigrammaton-cli.exe --js`・`--dump=layout` と host-*.exe（CPU の道だけ）。GPU・窓の判定に使わない | 参考。FAIL は Windows の不具合と決めない |
| 4 | 実機の Windows（ユーザーの host） | §7.3 | §7.3 |
| 5 | zedBSD | WS334 の変更で zedBSD の browser が壊れていない: `boot-test.sh`（login の画面）、browser の画面の確かめは WS074 の guest の道具（`plan/ws074/tests/browser-guest.sh`）を Q1 の割当で | 既存の判定 |

### 7.2 新しい host の試験（`plan/ws334/tests/`）

- `os-test.c`: libbrowser の OS の層の各関数（posix で host、win32 で cross build して実機・Wine）: socket の loopback、poll と waker、file の書き置き換え、path ↔ URL、時刻帯（固定の TZ）、乱数の長さ。
- `storage-test.c`: files の backend の表を直に（読み・書き・list・clear・壊れた記録・200 字を超える key・lock）、instance で cookie の期限・2 つの instance の分離・
  再起動の後の cookie と localStorage、今の `.ls` の file が読める事。
- `script-core-link.sh`: §3.2 の archive と `nm -u` の照合。
- `cross-build.sh`: §7.1 の段 2 を 1 本で（`make browser-win64`・`browser-winarm64` を流し、warning の数・export・依存の DLL を照合、最後の行 PASS・FAIL）。
- `libkeiland/win32/` の host でできる試験: 鍵の表（T1）・CF_HTML（T2）の単体試験（Linux の host の cc で、Win32 の header に依らない形の module にする）。

### 7.3 実機の Windows（ユーザーの host）

- 何を確かめるか: 起動、窓の枠（Windows 11 の snap の配置・暗い mode）、https の page（Windows の根）、文字の入力（英字・IME の日本語）、clipboard の copy、
  Vulkan の描画と予備の CPU の道（`--no-gpu` の引数で）、resize、DPI の違う 2 枚の monitor の移動、video（Vulkan Video と software）、localStorage と cookie の
  再起動の後の残り、2 つ目の起動。x86_64 と arm64 の両方。
- **最小の合格の組**（p015 の受け入れ。1 本の script が PNG・log・終わりの status で判定し、観測した物だけを PASS にする。レビュー #17）:
  - M1 `OmniGrammaton.exe` が起動し、同梱の `start.html` を Vulkan で描く（窓の PNG で文字が読める）。
  - M2 `OmniGrammaton.exe --no-gpu` と、`vulkan-1.dll` が見えない条件（別の folder に写した .exe と `SetDllDirectory` の無い環境、か GPU の無い VM）で CPU の道に落ちて描く。
  - M3 https の page（公開の site 1 つと、試験の CA の local の server）が開く。`Disallowed` の証明書の site を拒む。
  - M4 location の欄に英字と日本語（MS-IME）を入れて Enter で移る。候補の窓が欄の近くに出る（PNG）。
  - M5 page の text の欄に日本語を入れる（`browser_view_compose`・`_commit_text` の道）。
  - M6 再起動の後に cookie（期限付き）と localStorage が残る（試験の page が値を表示）。
  - M7 最大化の button に pointer を置いて snap の配置が出る（Windows 11、PNG）。暗い mode の題の帯。
  - M8 `omnigrammaton-cli.exe --js`・`--dump=layout` が Linux の host と同じ bytes を出す（O29 の binary の mode）。R4（setjmp と GC）は js の試験の数本で確かめる。
- 既存の libbrowser の試験のうち Windows で流す物: host-*.c の 91 本（8 本は `setenv(XDG_DATA_HOME)` か POSIX の path を使う: host-form・host-link・host-position・
  host-svg-text・host-svg-length・host-relayout・host-sticky・host-view。storage が backend になれば setenv は不要。host-link は file: の path の期待を Windows の形に）、
  `omnigrammaton-cli.exe` で `run-js-tests.py`・`run-test262.py`（test262 は `fetch-suites.sh` の取得物）・`run-html5lib-tree.py`・`run-url-tests.py`、
  `--render-gpu` の数枚と Linux の lavapipe の結果の比較（`gpu-compare.py`）。Interop 2025 の全部は流さない（時間。代表の 100 件程度、p015 で選ぶ）。
  python の runner は Windows の python で走らせる（`subprocess` の program の path を引数で渡せるかは p015 で確かめる。runner は WS074 の物なので変えるなら Q1・P8 と調整）。
- 担当の働き方（決定 11、問い U12）: Windows の host で要る物は §7.4。

### 7.4 Windows の host に要る物

| 物 | 用途 | 入手 |
| --- | --- | --- |
| OpenSSH server（Windows の機能） | 担当が SSH で build・試験を流す | Windows の「optional features」 |
| llvm-mingw の Windows 版（T-A と同じ版） | Windows の上の native の build | release の zip |
| git・make（GNU make）・python 3 | tree・build・試験の runner | Git for Windows（make は含まない → MSYS2 の make か llvm-mingw の `mingw32-make`） |
| GPU の driver（Vulkan の loader `vulkan-1.dll` を含む） | Vulkan・Vulkan Video | vendor の driver |
| Vulkan SDK（LunarG、任意） | validation layer での debug、`vulkaninfo` | LunarG の installer |
| 画面を撮る道 | 判定の PNG | PowerShell の script（`System.Drawing`）を対話の session で |

- 注意（未確かめ、p014 で確かめる）: SSH の session は対話の desktop でない（session 0 に近い形）ので、そこから起こした GUI の窓は画面に出ない。窓の試験は
  「利用者が login した時に走る」予定の task（`schtasks /Create /IT /RU <user>`）で対話の session に起動し、結果と PNG を file で回収する 1 本の script にする
  （AGENTS.md §9 の「1 本の script」）。headless の mode（`omnigrammaton-cli.exe`）は SSH の session で動く見込み（GPU を使う `--render-gpu` が session 0 で
  動くかは driver に依る）。
- 機械の電源・SwitchBot・割当は plan/tools.md の「実機」の規則で Q1 が決める。Windows の host を tools.md に登録するのは Q1（p014 で依頼）。

## 8. Phase（案）と見積もり

LW は Q1 の目安（8 LW ≈ 2〜3 時間）。「標準」= 標準廉価実装手順（docs/agent/spec-module-delegation.md）に回せる module。決定 15 の案 A の形。

| Phase | 目的 | 主な file | 受け入れ | 依存 | LW | 標準 |
| --- | --- | --- | --- | --- | --- | --- |
| p002 | build の土台: `sources.mk` への source の一覧の分け（libbrowser・libmedia・libtruetype・compat・libkeiland・browser の UI）、`apps/browser-win32/Makefile` の骨（toolchain の探し方・compat の header の写し・clean の守り、§6.9）と一番上の `browser-win64`・`browser-winarm64`、Vulkan の遅延の import の確かめ（§6.2）、libtruetype の 1 関数と compat の Windows の build、`plan/ws334/tests/cross-build.sh`（warning の数・`nm -u` の許す表・export・依存の DLL の許す表） | §9 の該当の行 | `make -f apps/browser-win32/Makefile` と `make browser-win64` の両方で 5 つの library が build でき、WS334 の書いた file に warning 0（compat・libtruetype の warning は数を記録）。zedBSD の libbrowser・browser と Linux の libtruetype の object の一覧が前後で同じ（R12） | p001 と U3 の答え | 6 | — |
| p003 | libbrowser の OS の層（posix・win32）と O13〜O24・O28〜O34 の直し（`vm/heap.c` の aligned の確保、`page/svg.c` の memmem を含む） | §9.1 の p003 の行 | 段 1 の件数が前と同じ。libbrowser の全部の .c の cross compile が warning 0 で、`nm -u` が許す表の中。`os-test.c` の posix の側が PASS | p002 | 9 | — |
| p004 | network の Windows（O1〜O10・O32・O33）、libmedia の wake の callback（M1）と `net_loader_ring` | `net/loader.c`・`net/http.c`・`net/tls.c`・`net/net.h`・`page/media.c`・`libmedia/engine.c`・`media.h` | 段 1、`run-loader-tests.py`・`run-http-tests.py`・`run-fetch-tests.py` の件数が同じ、net の cross compile が warning 0、`os-test.c` の socket・poll・waker の posix の側 | p003 | 10 | — |
| p005 | instance と API v4: instance、cookie の jar を instance へ（期限を含む）、Web Storage の領域を instance へ、storage の thread と queue、ストレージバックエンドの表と files の backend（cookie と LOCAL、v1 の読みと v2 の書き、OS の lock）、利用側の v4 への直し | `include/browser/browser.h`・`exports.map`・`net/cookie.c`・`net/http.c`・`page/storage.c`・`page/script.c`・`page/link.c`・`view/view.c`・`storage/*`（新）・利用側 | 段 1、`storage-test.c`（cookie と LOCAL）、2 instance の分離、今の `.ls` の読みと移行 | p004 | 12 | — |
| p006 | 永続の HTTP の cache（問い U5 の答えが「第 1 段で」の時。それ以外は後へ）: 索引、storage の thread での body の読み書き、上限の LRU | `net/loader.c`・`storage/*` | `storage-test.c` の cache の項、`run-loader-tests.py` の件数が同じ | p005 | 6 | — |
| p007 | script の core の archive と link の確かめ | `libbrowser/sources.mk`・`plan/ws334/tests/script-core-link.sh` | `nm -u` が libc・OS の層・libm だけ、archive だけの program で `--js` が走る | p003 | 2 | — |
| p008 | libmedia の Windows（M2・M4・M7・M9）と mediafile | `libmedia/*`・`mediafile/mediafile.c` | Linux の host の media の試験が同じ、cross compile warning 0 | p004 | 5 | — |
| p009 | browser の UI の準備: `browser_main` への分け、libkeiland の公開の header への 3 つの追加と Wayland の backend の実装、present の拡張の名前・CPU の道・鍵の `text`・時計（§4.8）、WS074 の `host-build.sh`・`host-shell.c` の追従 | `browser/*`・`include/keiland/*`・`libkeiland/ui/*`・`plan/ws074/tests/host-build.sh`・`host-shell.c` | zedBSD の browser・libkeiland の build が warning 0、`host-build.sh` の headless の試験の件数が同じ、zedBSD の guest の browser の確かめ（§7.1 段 5、Q1 の割当） | p005 | 5 | — |
| p010 | 鍵の表: scan code（set 1・拡張の bit）→ evdev の番号（`libkeiland/win32/keys-table.c`、Win32 の header に依らない module） | 上の 1 file と単体試験 | 表の単体試験（全 key、拡張の bit、Pause・PrintScreen の特別な列）。仕様の file と SHA-256、header の仕様の comment は p010 の前に設計の担当が用意する（手順 §4.1・§4.3） | p002 | 3 | ○ T1 |
| p011 | libkeiland の Win32 の最小の部分その 1: 共通に compile できる file の棚卸し（R9b）、platform-win32、kl_app・kl_window・loop・入力（N7 の最初の段）・N1 の custom frame と hit test・N8 の暗い mode・Vulkan の surface（遅延の import と loader の有無の確かめ）・CPU の表示・場所・ENOTSUP の stub、`apps/browser-win32/` の入口・単一の起動・resource・manifest | `libkeiland/win32/*`・`apps/browser-win32/*` | `make browser-win64` で libkeiland.dll と .exe が warning 0、export が Linux の一覧と同じ名前、Linux の Keiland の build と試験が同じ | p007・p008・p009・p010 | 14 | — |
| p012 | その 2: titlebar の controls を題の帯に（location の欄の編集を含む）、N2 の最初の段（IMM32）、N6 の clipboard（CF_HTML）、N3 の DPI | `libkeiland/win32/*` | warning 0。実機は p015 | p011 | 12 | ○ T2（CF_HTML の encode・decode の module） |
| p013 | OpenSSL の Windows の build（S-A）と Windows の根・Disallowed・Mozilla の根の合わせ | `userland/packages/security/openssl/Makefile.windows`（か script）・`net/tls.c`・`base/os-win32.c` | OpenSSL の DLL が同じ toolchain で build できる（tarball の SHA-256 照合）、設定の自動の読みが切れている事を `strings`・実行で確かめ、cross compile warning 0 | p004 | 5 | — |
| p014 | 配る形: `browser-win64-zip`、license の同梱、icon の道具（`tools/make-ico.c`）、版の情報 | `apps/browser-win32/*` | zip の中身が §6.2 の一覧、`objdump -p` の依存の DLL が許す表（Windows の system の DLL の名前の一覧と同梱の DLL）だけ | p012・p013 | 5 | ○ T3 ICO |
| p015 | 実機の Windows（x86_64）の第 1 回: Windows の host の準備（§7.4、Q1 の割当）、§7.3 の確かめを 1 本の script に。R4（setjmp と GC）の確かめもここ（Wine は任意なので、実機で初めて確かめる） | `plan/ws334/tests/windows-run.ps1` ほか | 最小の合格の組（§7.3 の M1〜M8）が全部 PASS。他の項目は観測した物を記録 | p014 | 8 | — |
| p016 | 後半の Windows の口（§4.9）: N2 の TSF、N6 の drag と drop、N7 の DirectManipulation と pen、N8 の accent、N1 の Mica、arm64 の実機（T-A） | `libkeiland/win32/*` | 実機で IME の再変換・周りの文字・drag・touchpad の慣性、arm64 で M1〜M8 | p015 | 14 | — |
| p017 | libbrowser の試験を Windows で（§7.3 の一覧、runner の期待の差 O28・O29 の扱い） | — | 試験の件数の記録（zedBSD・Linux の件数との差と理由） | p015 | 5 | — |
| p018 | 規約の全文の見直し（WS334 が変えた C の全行、触れた block の開発の管理情報の comment の直し） | — | conformance-reviewer の結果 | p016・p017 | 4 | — |

- 計 約 131 LW（p006 を後に回すと 125 LW）。レビューの前の見積もり（112 LW）から、p005 の分け（cache を p006 へ）、p016 の大きさ、規約の Phase の独立、p003 の追加の直し
  （O28〜O34）で増えた。予備の案 B に切り替える時は p009・p011・p012・p016 を「libkeiland に依らない共通の shell（10 LW）」「Windows の標準の部品の層（14 LW）」
  「Windows の層の第 2 段（10 LW）」に置き換える（§4.2）。N9（chrome の UIA）は WS334 の後か WS234 で（page の中身の accessibility は範囲の外）。
- 並行できる組: p007・p010 は p002・p003 の後に並べられる。p008 は p004 の後。p013 は p004 の後に p005〜p012 と並行。
- **標準廉価実装手順に回せる module**（外部の標準で振る舞いが決まり、host で単体試験できる物）:
  - T1 鍵の表: Microsoft の「Keyboard Scan Code Specification」（scan code の set 1）と USB HID Usage Tables（Keyboard の page 0x07）の対応、Linux の
    evdev の番号（`input-event-codes.h` の値をデータとして引用、guardrail の GPL の値の規則）、W3C UI Events KeyboardEvent code の名前。
  - T2 CF_HTML の形: Microsoft の「HTML Clipboard Format」（header の `Version`・`StartHTML`・`EndHTML`・`StartFragment`・`EndFragment` の offset）の encode と decode。
  - T3 ICO の writer: Microsoft の ICO・BMP の形（`ICONDIR`・`ICONDIRENTRY`、PNG の entry）。
  - 前の版の T4（Win32 の誤り → errno の表）は、写し方が標準で決まらない設計の判断なので外した（レビュー #17）。WS327 §5.3 の表を写す普通の作業として p003 に入れる。
  - 手順の §4.1・§4.3 が求める物（仕様の file と SHA-256、仕様の comment の header、骨格、試験の道具）は p001 では作っていない。T1〜T3 の実装の Phase の前に、
    設計の担当がそれを作る小さな設計の Phase（各 1 LW 程度、上の表の LW に含めた）を置く。
  - それ以外（OS の層・storage・窓・IME・DWM）は設計を探りながら作る・UI なので普通の Phase（手順の §1 の条件を満たさない）。

## 9. 変える file の一覧（P8・WS074 との調整のため）

### 9.1 libbrowser（`userland/desktop/libbrowser/`）

「P8 と重なる見込み」は WS074 がよく変える directory（page・css・bind・view・net/loader）を中・高にした目安（P8 の予定は聞いていない）。

| file | Phase | 何を | P8 と重なる見込み |
| --- | --- | --- | --- |
| `sources.mk`（新） | p002 | source の一覧を Makefile から移す（中身は同じ） | 高（P8 が source を足す時に同じ行。p002 の merge の後は P8 は sources.mk に足す） |
| `Makefile` | p002・p003・p005 | `sources.mk` を include、os-posix.c・storage の source | 中 |
| `base/os.h`・`base/os-posix.c`・`base/os-win32.c` | p003 | 新 | なし |
| `base/base.h`・`base/buffer.c`（か新しい `base/memmem.c`） | p003 | `wb_memmem`（O31） | 低 |
| `base/file.c` | p003 | 中身を OS の層へ | 低 |
| `vm/heap.c` | p003 | `posix_memalign`・`free` の 6 か所を `wb_os_aligned_*` へ（O30） | 低 |
| `net/url.c` | p003 | file: の path ↔ URL を OS の層へ（:412-459） | 低 |
| `page/page.c` | p003 | `realpath`（:284） | 中 |
| `page/svg.c` | p003 | `memmem`（:1968） | 中 |
| `view/view.c` | p003・p005 | `getcwd`（:436）、instance、`browser_free`、版の確かめ（:321） | 中 |
| `js/builtin_date.c` | p003 | 時刻帯（:289・:1459・:1662、略号の表 :200-212 は POSIX の側に残す） | 低 |
| `js/builtin_typed.c` | p003 | 乱数（:1377） | 低 |
| `bind/environment.c` | p003 | 乱数（:1228）、navigator.platform（:229）、os-name の include（:29） | 中 |
| `text/text.h`・`text/font.c` | p003・p005 | 既定の font の path、bold・fallback_mono（:39-41、:74・:80） | 低 |
| `css/values.c` | p003 | `(long)` の判定（:7012-7013）を 64 bit の整数か double の比較に | 中（1 関数） |
| `net/net.h` | p004・p005 | User-Agent、socket の型、TLS の wants の印（:184-186）、`net_loader_ring`、cookie の jar を引数に | 中 |
| `net/loader.c` | p004・p006 | pipe・poll・socket・thread の名前・watch → ring（p004）、永続の cache の 2 段目（p006） | 中〜高（2,482 行の大きな file） |
| `net/http.c` | p004・p005 | socket・poll・`MSG_NOSIGNAL`（p004）、`net_cookie_header`（:426）・`net_cookie_store`（:1010）に jar（p005） | 低 |
| `net/tls.c` | p004・p013 | library の名前・`wb_os_library`・socket の型・errno（:302・:482）・poll の印、Windows の根と Disallowed・既定の path を呼ばない | 低 |
| `net/cookie.c` | p005 | jar を instance へ、期限、persistence | 低 |
| `page/media.c` | p004 | wake の callback（:132・:698）を open の引数と `net_loader_ring` へ | 低 |
| `page/storage.c` | p005 | 領域を instance へ、file の IO を backend へ（XDG の削除）、:8-24 の comment を書き直す | 低 |
| `page/script.c` | p005 | cookie の呼び出し（:1612・:1639）と `net_http_fetch_send`（:575）に jar、storage の表 | 中 |
| `page/link.c` | p005 | `net_http_fetch`（:260・:370）に jar | 中 |
| `page/network.c` | p005 | loader に instance | 低 |
| `page/page.h` | p005 | page に instance の pointer | 中 |
| `storage/storage.h`・`storage/instance.c`・`storage/files.c`・`storage/records.c`・`storage/thread.c` | p005・p006 | 新 | なし |
| `exports.map` | p005 | `browser_instance_*`・`browser_storage_*`・`browser_free` | 低 |

- 公開の header `userland/desktop/include/browser/browser.h`（p005、v4。`LC_NUMERIC` の約束の comment も）。
- 変えない: html・dom・xml・layout・paint・image・bind（environment.c を除く）・js（date・typed を除く）・vm（heap.c を除く）・tools。Vulkan の遅延の import が
  働かない時だけ、予備の `paint/vulkan-win32.c`（新、Windows の build だけ）が加わる（§6.2）。
- 触れる file の開発の管理情報の comment（WS・Phase の番号など。例: `page/media.c`・`view/view.c`・`page/storage.c` にある）は、guardrail の規則で、触れた block を
  p018 の規約の見直しで直す（Phase の作業の中で新しく書かない）。
- 調整の案（Q1 へ）: p002 の `sources.mk` は P8 の source の追加と必ず当たるので、P8 の返却の合間に小さく入れる。p003 と p005 の前に、P8 に上の表の「中・高」の
  file を触る予定を聞き、WS334 の commit を小さく分けて早く main に入れる。`net/loader.c` は p004 と p006 を P8 の手が空いた時に入れる案もある。

### 9.2 libbrowser の外

「P8 と重なる見込み」: browser の UI（`userland/desktop/browser/`）は WS074 の物で、2026-10-30・31 にも変わった（中）。WS074 の試験の file は高。

| path | Phase | 何を | P8 と重なる見込み |
| --- | --- | --- | --- |
| `Makefile`（一番上） | p002 | `browser-win64`・`browser-winarm64` ほか（§6.9）、`ZEDBSD_CONFIG_OPTIONAL_GOALS` | 低 |
| `apps/browser-win32/*`（新） | p002・p011・p014 | §4.7・§6.9 | なし |
| `userland/desktop/browser/main.c`・`shell/*`・`Makefile`・`sources.mk`（新） | p002・p005・p009 | 一覧の分け、v4、§4.8 | 中 |
| `userland/desktop/picture/png-write.c` | — | 変えない（Windows の build に入れるだけ） | — |
| `libtruetype/companion.c`・`libtruetype/sources.mk`（新）・`Makefile`・`Makefile.linux` | p002 | 1 関数、一覧の分け | 低 |
| `libpng-compat`・`libjpeg-compat`・`libgif-compat`・`userland/base/libz-compat` の `sources.mk`（新）と Makefile、`libgif-compat/decode.c`（`O_BINARY`） | p002 | 一覧の分け・1 行 | 低 |
| `libmedia/engine.c`・`media.h`・`exports.map`・`vkvideo-runtime.c`・`vkvideo.c`・`picture.c`・`media-os.h`・`media-os-posix.c`・`media-os-win32.c`（新）・`sources.mk`（新）・`Makefile` | p002・p004・p008 | §5 | 低 |
| `mediafile/mediafile.c` | p008 | M4 | 低 |
| `include/keiland/keiland_window.h`・`keiland_app.h`・`libkeiland/ui/window.c`・`ui/app.c` | p009 | `kl_window_vulkan_extensions`・`kl_app_storage_directory`・`kl_data_directory`（加えるだけ、§4.5） | 低 |
| `libkeiland/win32/*`（新）・`libkeiland/sources.mk`（新）・`libkeiland/Makefile`・`Makefile.linux`・`libkeiland/exports.py` | p010〜p012・p016 | §4.4・§4.5 | なし〜低 |
| `userland/tests/browser-probe/main.c` | p005 | v4 | 低 |
| `plan/tools/browser-component/client.c` | p005 | v4（plan/tools は Q1 の物、Q1 に頼む） | — |
| `plan/ws074/tests/host-view.c`・`host-form.c`・`host-sticky.c`・`wpt-ready-render.c`・`host-position.c`・`host-link.c`・`host-build.sh`・`host-shell.c`・`list-sources.sh` | p002・p005・p009 | v4、storage の source、`sources.mk` を読む、`browser_main`、`setenv(XDG_DATA_HOME)` の扱い（WS074 の物。Q1・P8 と調整） | 高 |
| `userland/packages/security/openssl/Makefile.windows`（か script） | p013 | 新 | なし |
| `plan/tools/keiland-os-boundary/check.sh` | — | `apps/`・`libkeiland/win32/` を見る範囲に（R14。Q1 に頼む） | — |

## 10. 出典と引用

- 書く code はどれも zedBSD の Zlib。他の実装（Chromium・Ladybird・Qt・Firefox・curl・Wine）の code・comment・構造・symbol の名前を写さない。
- 手順を参考にする所は、その block の直前に複数行の comment で「Procedure referenced: <版> <文書か path> (<節・関数>)」を書く（guardrail の出典の規則。file の
  先頭に書かない。source に WS・Phase の番号を書かない）。予定の出典:
  - Win32・DWM・IMM32・TSF・UIA・WinSock・BCrypt・CryptoAPI・ICO・CF_HTML・scan code の仕様: Microsoft Learn の該当の頁（版は取得の日）。
  - Vulkan: Khronos の Vulkan の仕様（`VK_KHR_win32_surface`・Video の拡張）、Vulkan-Headers（Apache-2.0・MIT。build に使い配らない）、Vulkan の loader の export の一覧。
  - evdev の番号: Linux の `include/uapi/linux/input-event-codes.h`（GPL-2.0 WITH Linux-syscall-note。値をデータとして「Values referenced:」で引用、名前・構造は写さない）。
  - RFC 6265（cookie の storage model §5.3）、RFC 9111（HTTP の caching）、W3C UI Events KeyboardEvent code Values。
  - OpenSSL の API（Apache-2.0、関数の名前は library の API として呼ぶ。今の `net/tls.c` の扱いと同じ）。
- この設計の文書の他の実装についての記述（Chrome の Views と TSF、Ladybird の Qt、node.js と V8・libuv・d8）は P7・Q1 の知識で、出典の取得はしていない。
  設計の判断はそれらの今の版の細部に頼らない（§12 の未確かめ）。

## 11. 選ばなかった案

| # | 案 | 選ばない理由 |
| --- | --- | --- |
| A1 | libbrowser が libkeiland の `kl_thread`・`kl_socket` を使う | WS327 の決定 2（libbrowser は単体の library、wrapper を使わない） |
| A2 | libbrowser の OS の差を mingw の POSIX の模倣（winpthreads + 自前の `pipe2`・`poll` の shim）だけで埋める | pipe・poll・fcntl・`/dev/urandom`・`tm_gmtoff`・file の path は模倣が無く、shim の置き場が結局 OS の層になる。thread だけは winpthreads を使う（§3.4） |
| A3 | `#ifdef _WIN32` を各 file に散らす | OS の差が libbrowser 中に散り、P8 の変更と競合しやすい。guardrail の WS103（OS 固有は OS ごとの module） |
| A4 | network の transport を program が登録する backend にする（storage と同じ形） | 公開の API が大きくなる。決定 9 の 3 つ（rendering・script・storage）に無い。OS の層で足りる |
| A5 | libkeiland の Wayland の実装の中に window system の継ぎ目（backend の表）を作る | `struct kl_window` の Wayland の field を 14 の file が直に使い、Keiland の今の開発と競合し、zedBSD の回帰の危険が大きい。同じ契約の別の実装（§4.6）で足りる |
| A6 | Windows の最初の frontend を Qt6 にする（`apps/browser-qt` を先に） | DLL と LGPL の約束、Qt6 の x86_64・arm64 の cross build。Qt の frontend は他の OS に出す時に（§4.3） |
| A6d | libkeiland の backend を Qt6 にする（libkeiland-qt6） | §4.3 |
| A6b | 案 B（Windows の標準の部品の chrome を `apps/browser-win32/` に別に書く） | 決定 15 で案 A。B は予備（§4.2 の切り替えの印） |
| A6c | （案 B の時）共通の shell を別の DLL（`libbrowser-shell.dll`）にする | 公開の ABI が 1 つ増える。frontend ごとに static に compile すれば足りる（source は 1 か所） |
| A7 | Direct3D 12 で描く | libbrowser の renderer と libmedia が Vulkan。2 つ目の GPU の道を作る理由が無い |
| A8 | ストレージバックエンドが cookie の照合・cache の鮮度まで受け持つ | backend ごとに RFC の論理を重ねて書く事になる。論理は libbrowser、backend は記録の保存だけ |
| A9 | backend の read が malloc した memory を返す | DLL ごとの heap（§2.11）。sink に書き足す形 |
| A10 | script の core に公開の header を今作る | §3.3（外の embedder が出てから） |
| A11 | MSVC | §6.1 T-D |
| A12 | SChannel を第 1 段に | §6.6 |
| A13 | memory の HTTP の cache を instance で共有する | view ごとの network の thread が entry の解放と参照で競う（`loader.c:1942`・`:2403`）。memory の cache は loader ごとのまま、永続の索引だけを instance に（§3.5.4、レビュー #3） |
| A14 | backend の表を page・network の thread から lock の下で直に呼ぶ | disk の IO を lock の下で待つ（document.cookie が disk を待つ）。storage の thread の write-behind にした（§3.5.2） |
| A15 | lock の file の有無で 2 つ目の process を断る | 落ちた process の残した file で永久に memory だけになる。OS の lock にした（§3.5.5） |
| A16 | `-mcrtdll=ucrt` を msvcrt の版の gcc に付ける | libgcc・libmingwex が msvcrt で build されていて混ざる。UCRT の版の toolchain にした（§6.1） |
| A17 | `vulkan-1.dll` を普通の import で link | loader の無い機械で .exe が起動しない。遅延の import にした（§6.2） |

## 12. 危険と未確かめ

| # | 何 | 扱い |
| --- | --- | --- |
| R1 | Wine で Vulkan が動かない見込み（決定 11） | Wine は任意（§7.1 段 3）。判定は実機 |
| R2 | SSH の session から GUI を出せない | §7.4 の task の形。p014 で確かめる |
| R3 | winpthreads の `pthread_condattr_setclock(CLOCK_MONOTONIC)` が効かない | p004 の最初に小さな program で確かめ、効かなければ loader の待ちを OS の層に |
| R4 | mingw の x64 の `setjmp`・`longjmp` の SEH の巻き戻しと GC の stack の走査（O26・O27） | p003 で host-*.exe（js・vm の物）を実機・Wine で流す。問題なら `__builtin_setjmp` か巻き戻しを切る形（`_setjmp(buf, NULL)` の相当）を OS の層に |
| R5 | Windows の Vulkan の driver の Vulkan Video の対応 | software の H.264 に落ちる（今の順）。§7.3 で両方を確かめる |
| R6 | libbrowser は 1 process・sandbox 無し。Windows の利用者は一般の web を見る | WS334 の範囲の外。WS074 と同じ危険。実機の試験は既知の site だけ。sandbox は Future Work の候補 |
| R7 | P8 との競合（§9.1 の「中・高」） | Q1 の調整（§9.1 の案） |
| R8 | 高 DPI の中身（§4.5） | 問い U4 |
| R9 | （予備の案 B に切り替える時だけ）今の Keiland の shell の中の、共通にできる論理と kl に結び付いた所の境が未確かめ（`shell.c` 1,462 行の細部） | 切り替えの時に関数ごとの表を作る |
| R9b | libkeiland の共通に compile できる file の範囲が未確かめ（§2.12） | p010 の最初の棚卸し。少なければ p010 の LW が増える（Q1 に報告） |
| R12 | `sources.mk` への分けで zedBSD の package の build が変わる | p002 で zedBSD の libbrowser・browser の object の一覧が前後で同じ事を照合 |
| R13 | `strtod` の locale（O34）: libkeiland の将来の Qt・GTK4 の backend や他の library が `setlocale(LC_ALL, "")` を呼ぶと、CSS・SVG の小数の読みが壊れる | browser.h に「`LC_NUMERIC` は "C" のまま」を書き、libkeiland の backend は `LC_NUMERIC` を変えない。locale に依らない読みへの置き換えは WS074 と調整 |
| R14 | `apps/` と `libkeiland/win32/` は OS の境の checker（`plan/tools/keiland-os-boundary/check.sh:205` は `git ls-files userland plan` を見る）の外。P8 などが libbrowser に POSIX の呼び出しを足しても Windows の build が壊れるまで気付かない | `cross-build.sh` の `nm -u` の許す表で機械で見つける。checker を `apps/` に広げるのは Q1 に頼む（plan/tools は Q1 の物）。WS074 の Phase の確かめに `make browser-win64` を足す案を Q1 に |
| R10 | Chrome・Ladybird・node.js の記述の細部（§10） | 判断はそれに頼らない |
| R11 | 公開の API v4 は WS074 の試験の file と browser-component の client を変える | §9.2、Q1 の調整 |

## 13. ユーザーへの問い（本文は [user-questions.md](user-questions.md)）

統括の Q1 と紛れないよう、問いの番号は U1〜U15 にした。前の版の「案 A か B か」は決定 15 で決まり、「題の帯の見た目」は決定 15 と §4.4 の N1（題の帯に controls）で決まったので外した。

U1 libkeiland の Windows の backend（Win32 直の確かめ）と全体の移植の置き場（WS234）／ U2 browser の UI の source の置き場 ／ U3 toolchain（llvm-mingw の導入）／
U4 高 DPI ／ U5 HTTP の cache を disk に残すか ／ U6 Windows の font ／ U7 libtruetype・gif の小さな直し ／ U8 User-Agent と navigator.platform ／
U9 名前（zedBSD の上）・icon・発行者 ／ U10 TLS ／ U11 配布と署名 ／ U12 Windows の host での担当の働き方 ／ U13 script の core の公開の header ／
U14 cookie の暗号の保存 ／ U15 決定 9 の読み（instance に登録するのは保存だけ）。
make の target の名前（`browser-win64`・`browser-winarm64`）は §6.9 の案で、問いにしない（Windows の慣例の名前に合わせた）。

## 14. design-reviewer の指摘と反映

- 2026-11-01、design-reviewer が a80138ca4 の版を読み、高 4・中 14・低 6 の指摘を返した。全部を反映した（決定 15 で予備になった案 B に関わる物は §4.2 の記録へ）。
  指摘ごとの扱いと直した節は [design-review.md](design-review.md)。主な直し: `vm/heap.c` の `posix_memalign`（O30）、Vulkan の遅延の import（§6.2）、memory の HTTP の
  cache は loader ごとのまま・storage の thread の write-behind と lock の順（§3.5）、libmedia の起こしを open の引数と `net_loader_ring` に（M1）、M7 は Pi 4 の回避、
  key の衝突と `.ls` の移行（§3.5.3）、UCRT の版の toolchain と Windows 10 1903（§6.1）、TLS の根と OpenSSL の設定（§6.6）、Phase の分け直し（§8）と file の一覧（§9）。
