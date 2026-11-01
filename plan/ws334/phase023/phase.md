<!-- awesome-plan project=zedbsd record=ws334-p023 -->

# ws334-p023 統合: instance と API v4（cookie の jar・Web Storage を instance へ、storage の module をつなぐ）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §3.5〜§3.8、§2.5、§9.1・§9.2。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1847-i01、P5。結果は末尾）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: `libbrowser.dll` の link と `.def` の export の照合は無い。libbrowser の全部の object を static library `libbrowser.a` にし（p036 の `WIN32_STATIC_LIBRARY` の形）、cross-build.sh の libbrowser の `nm -u` の照合を全部の object に広げる。下の本文の `libbrowser.dll`・`.def` はこの通りに読み替える。
- 依存: p010（records）・p011（files）・p012（queue）・p022。
- 担当の想定: Opus（phase-runner）を 1 回。使う module の約束は `include/browser/storage.h`・`storage/records.h`・`storage/files.h`・`storage/queue.h` の comment。公開の API の形はこの phase.md の下の通り（design §3.8 を確定させた物）。

## 調整（始める前）

- 公開の `browser.h` の版を上げ、利用側（WS074 の試験の file を含む）を直すので、Q1 に file の一覧（下の「利用側」）を送り、P8 の作業と当たらない時に始める。`plan/tools/browser-component/client.c` は plan/tools なので Q1 に直しを頼む（差分を作って渡す）。

## 公開の API（`userland/desktop/include/browser/browser.h`、v4。この形で書く）

- `#define BROWSER_API_VERSION 4`。`#include <browser/storage.h>` を browser.h の include の所に。
- 足す:
  ```c
  /* An instance of the engine: what its views share and what it remembers (cookies, the HTTP cache, Web Storage). */
  struct browser_instance;

  struct browser_instance_options {
  	int version;                                  /* BROWSER_API_VERSION */
  	const struct browser_storage_calls *storage;  /* the backend, or NULL to remember nothing */
  	void *storage_context;                        /* the backend's context, owned by the instance from create on */
  	size_t cache_limit;                           /* bytes of the persistent HTTP cache; 0 for 256 MiB */
  };

  int browser_instance_create(const struct browser_instance_options *options, struct browser_instance **instance);
  int browser_instance_destroy(struct browser_instance *instance);
  void browser_free(void *memory);
  ```
  - comment（各関数の前に、design §3.6・§3.7 から）: create は options の version が違えば ENOTSUP、storage の version が違えば ENOTSUP、失敗の時も storage の close を 1 回呼ぶ、cookie の space の読みが失敗しても空の jar で作る（ENOMEM だけは失敗）、作った thread がその instance と view を使う。destroy は view が残れば EBUSY で何もしない、それ以外は storage の queue を流し切り close を呼んで 0。`browser_free` は `browser_view_dump` などが返した memory を放す（NULL は何もしない）。
- `struct browser_view_options` の最後に `struct browser_instance *instance;`（NULL は process に 1 つの既定の instance = 何も保存しない、once で作り process の終わりまで残す）。
- `struct browser_fonts` の最後に `const char *bold;` と `const char *fallback_mono;`（NULL は既定: `wb_os_data_directory` + "/fonts/keiland-bold.ttf"・"/fonts/keiland-fallback-mono.ttf"、今の `KEILAND_FONT_BOLD`・`KEILAND_FONT_FALLBACK_MONO` と同じ file）。text/font.c の companion の path をこれに替える。
- `browser_view_dump` の comment の「呼び手が free する」を「`browser_free` で放す」に。
- file の始めの comment に約束を 1 段落: libbrowser を使う program は `LC_NUMERIC` を "C" のまま保つ（数の読みが locale に従うため。design O34）。
- `libbrowser/exports.map` の global に `browser_instance_*;`・`browser_storage_*;`・`browser_free;`。

## libbrowser の中

1. **instance**（新 `storage/instance.h`・`storage/instance.c`、private）: `struct browser_instance` = cookie の jar（下の 2）、localStorage と sessionStorage の領域の一覧（今の `page/storage.c` の static の `storage_areas` をここへ）、`struct wb_storage_queue *queue`、cache_limit、view の数（`view_count`、page の thread だけが触る）。create: options を確かめ、`wb_storage_queue_open`（失敗なら storage の close を呼んで返す）、jar を作り、COOKIES の space を `wb_storage_queue_list` → 各 key を `wb_storage_queue_read` → `wb_storage_cookie_decode` → 期限（`expires` ≤ `wb_os_wall_ns()/1000000`）の過ぎた物と decode の失敗は捨てて `wb_storage_queue_remove`、残りを jar に。destroy: view_count が 0 でなければ EBUSY、`wb_storage_queue_close`、jar と領域を放す。既定の instance（view の options の instance が NULL）は **thread ごと**に 1 つ: `pthread_key_t` に持ち、その thread の最初の要求で backend 無しで作る（「1 つの instance とその view は 1 つの thread で使う」約束を保つため。今の process に 1 つの jar を共有する挙動は同じ thread の view の間でだけ保たれる。違う thread の NULL の view の間で cookie を共有しなくなる変化を結果に書く）。key の destructor は thread の終わりに view_count が 0 なら destroy、0 でなければ残す（process の終わりまで）。
2. **cookie の jar**（`net/cookie.c`・`net/net.h`）: static の `cookie_jar`・`cookie_count`・`cookie_lock` を `struct net_cookie_jar`（配列 COOKIE_MAX = 512、数、mutex、queue の pointer）に移す。`struct cookie` に `int64_t expires`（ms、session の cookie は -1）・`created`・`used`、`same_site`。Expires・Max-Age を保つ（今は過去の削除だけ）: Max-Age が優先（RFC 6265 §5.3 の 3）、`expires` = 今 + Max-Age 秒。期限の過ぎた cookie は header・string を作る時に捨てる。`net_cookie_store`・`_header`・`_store_script`・`_string` の最初の引数に `struct net_cookie_jar *jar`。期限のある cookie を足す・替える時は `wb_storage_cookie_encode` → key（`wb_storage_cookie_original_key` → `wb_storage_key_text`、collision の扱いは下の 4）→ `wb_storage_queue_write(COOKIES)`、消す時は `wb_storage_queue_remove`。queue の関数は待たない物だけを jar の lock の中で呼んでよい（queue.h の lock の約束）。session の cookie は保存しない。期限付きの cookie が同じ domain・path・name の session の cookie に置き換わった時は、その record を `wb_storage_queue_remove` する（残すと再起動で戻るため）。`used`（最後に使った時）は memory でだけ更新し、送る・読むたびには書かない（毎回の要求で disk に書かないため）。record の `used` は保存・置き換えの時の値。SameSite は今の cookie.c が読まないので、この Phase では属性を読まず record の flags は `WB_STORAGE_COOKIE_SAME_SITE_UNSET` のまま（読みは範囲の外。将来の Phase）。512 を超えて古い物を捨てる時、それが保存した物なら remove。
3. **Web Storage**（`page/storage.c`）: 領域を instance の物に（関数は page から `page->instance` を引く）。localStorage の file の IO（XDG の path・`mkdir`・`getpid`・`rename`・`unlink`、:33-34・:663-667・:691-720・:872-881）を全部消し、読みは `wb_storage_queue_read(LOCAL, key)` → `wb_storage_local_decode`（v1 は origin を持たない、v2 は origin が領域の origin と同じことを確かめる）、書きは `wb_storage_local_encode`（v2）→ `wb_storage_queue_write`。key は origin の bytes の `wb_storage_key_text`（短い origin は今の file の名前と同じ hex、files の backend の `.ls` で今の file がそのまま読める）。file の始めの comment（:8-24）を新しい形に書き直す（XDG の段落を消し「instance の backend に保存」）。`<sys/stat.h>`・`<unistd.h>` を外す。
4. **key の衝突**（hash の形の key だけ。records.h の約束）: 読み: collision 0 から順に read し、found 0 で「無い」、decode した original が自分の物なら「それ」、違えば次（最大 16 まで。超えたら無い扱い）。書き: 同じ順で、無い key か自分の original の key に書く。queue の read は待つので、jar の lock を持ったまま読まない（cookie の書きの key は jar の中に覚えておく: `struct cookie` に `char key[BROWSER_STORAGE_KEY_MAX + 1]`、instance を開いた時の読みで分かる。新しい cookie は lock の外で key を決めてから lock の中で足す）。
5. **呼び出しの直し**: `net/http.c:426`・`:1010`、`page/script.c:575`・`:1612`・`:1639`、`page/link.c:260`・`:370`、`page/network.c`（loader に jar）、`page/page.h`（page に `struct browser_instance *instance`）、`view/view.c`（options の instance、`view_count` の増減、version の確かめ :321 を 4 に、`browser_free`）。
6. **誤りの知らせ**: `wb_storage_queue_take_error` を view の `browser_view_process` の始めで読み、0 でなければ page の console に 1 行（"storage: could not save (<strerror>)"、view ごとに 1 回）。
7. `sources.mk`: `LIBBROWSER_ENGINE_SOURCES` に `storage/records.c`・`storage/files.c`・`storage/queue.c`・`storage/instance.c` を足す（Windows の Makefile は win32 の物を）。

## 利用側の v4

- `userland/desktop/browser/main.c`・`browser/shell/*`: instance を作る。shell（窓の mode）は `browser_storage_files_open(<storage の directory>)` の backend、directory は p027 で `kl_app_storage_directory` から。p023 では今の localStorage と同じ親 `$XDG_DATA_HOME/keiland/browser`（無ければ `$HOME/.local/share/keiland/browser`）を browser の UI の中で求める（libbrowser は環境を読まない）。headless の mode（`--dump`・`--run`・`--js`・`--render`）は backend 無し（今は headless でも localStorage の file を書くが、試験の再現のため書かない形にする。この変化を結果に記録）。files の open が EBUSY なら backend 無しで続け、stderr に 1 行。
- `userland/tests/browser-probe/main.c`、`plan/ws074/tests/host-view.c`・`host-form.c`・`host-sticky.c`・`wpt-ready-render.c`（options の version 4、instance NULL）。`host-position.c`・`host-link.c`・`host-svg-text.c`・`host-svg-length.c`・`host-relayout.c` の `setenv(XDG_DATA_HOME)` は不要になる（消すかは P8 の判断、Q1 経由）。
- `plan/tools/browser-component/client.c` は Q1 に差分を渡す。

## 試験（受け入れ）

- 新しい host の試験 `plan/ws334/tests/storage-test.c`（この Phase で書く。libbrowser の source と host の cc で、`plan/ws334/tests/storage-test.sh` が build して流す、最後の行 `storage-test: PASS`）:
  - files の backend の instance で cookie（Max-Age と Expires、session）を page の script と HTTP の header（host の `python3` の小さな server か `net_cookie_store` を直に）で入れ、instance を destroy して作り直すと期限付きだけが残る、期限の過ぎた物は残らない。
  - localStorage を入れて作り直すと残る。今の v1 の `.ls` の file を directory に置くと読め、次の書きで v2 になる。
  - 2 つの instance（別の directory）が cookie と localStorage を分ける。同じ directory の 2 つ目の instance は EBUSY で backend 無しに。
  - view の残る instance の destroy は EBUSY。backend の close が 1 回だけ呼ばれる（fake の backend で数える）。fake の write の EIO で console に 1 行。
- 段 1 の件数が前と同じ（host-build の plain・asan、run-js-tests、browser-component、network の試験）。setenv を消した試験は同じ結果。
- `run-unit.sh all` PASS、`cross-build.sh` PASS（`net/cookie.c`・`page/storage.c` を含む libbrowser の全部が両方の arch で warning 0、`libbrowser.dll` の link と export の照合はここから: `.def` は `exports.map` の glob を object の `nm` で展開）。
- zedBSD amd64 の libbrowser・browser の build が warning 0。zedBSD の guest の browser の確かめ（localStorage が再起動で残る）は Q1 の割当で（design §7.1 段 5、`plan/ws074/tests/browser-guest.sh`）。
- `git diff --check`。

## 変えてはいけない物

- module の header と .c、試験の道具（modules.sh・run-unit.sh・check-skeleton.sh・unit/）。header の不足は止めて Q1 に。
- HTTP の cache の永続（p024）。HAL・toolchain・共有の build/。rm をしない。push しない。

## 終わりの条件

- 上が全部 PASS（件数の前後の表）。file の群ごとの commit。SHA・command と最後の行・未実施・headless の localStorage の変化を Q1 に。ws.md の行を「実装済み・試験 PASS」に。

## 結果（2026-11-01、P5、q1847-i01、基点 15cd956cf）

commit（`git commit -m WIP -- <path>`、file の群ごと）:

| SHA | 中身 |
| --- | --- |
| 6b1d17d45 | libbrowser: 新 `storage/instance.h/.c`、`net/cookie.c`（jar・期限・永続）・`net/net.h`・`net/http.c`・`net/loader.c`（`net_loader_set_cookies`）、`page/storage.c`（instance の領域・backend へ）・`page/page.h/.c`・`page/network.c`・`page/link.c` と page_fetch の呼び手 8 file、`view/view.c`、`text/text.h/font.c`（companion）、`sources.mk`・`Makefile`・`exports.map`、公開の `browser.h`（v4） |
| 6696975c2 | 利用側: `browser/main.c`（`browser_free`）・`browser/shell/shell.c`（files の backend の instance）、`plan/ws074/tests` の host-build.sh（storage.h の link 1 行）・host-view/form/sticky（`browser_fonts` の memset 1 行、`browser_free`）・host-data-url/file-subresource/resource（page_fetch の jar に NULL）、`plan/tools/browser-component/client.c`（fonts の memset 1 行、Q1 が範囲に入れた） |
| c211be4ae | `apps/browser-win32/Makefile`（libbrowser.a に libbrowser の全部）、`plan/ws334/tests`: 新 storage-test.c・storage-test.sh・stage1.sh、cross-build.sh（3b を libbrowser.a 全部に、3c 公開の API の照合）・allowed-imports.txt（`strcpy`）・fetch-main.c・fetch-wine.sh・net-ring-test.sh |

前後の件数（`sh plan/ws334/tests/stage1.sh TREE plain|asan`。前は `git archive 15cd956cf` を `build/tmp/p023-base`、後は `git archive c211be4ae` を `build/tmp/p023-tip` に展開して流した）:

| 確かめ | 前（15cd956cf） | 後（c211be4ae） |
| --- | --- | --- |
| `host-build.sh plain` / `asan` | exit 0、warning 0、built 94 / 同じ | 同じ / 同じ |
| host-*（plain・asan、各 600 s） | 75 exit 0・17 その他（引数の要る試験） | 75・17、試験ごとの exit の値が plain・asan とも全部同じ（diff なし） |
| `run-js-tests.py`（plain・asan） | 17/17 | 17/17 |
| `run-loader-tests.py`（plain・asan） | 11/11 | 11/11 |
| `run-http-tests.py`（plain・asan） | 15/15 | 15/15（cookie の case を含む） |
| `run-http-tests.py --async`（plain・asan） | 19/20（FAIL は async-sheets） | 19/20（同じ async-sheets） |
| `run-fetch-tests.py`（plain・asan） | 38/38 | 38/38 |
| `plan/tools/browser-component/run.sh plain` / `asan` | 83 checks PASS | 83 checks PASS |

新しい試験と他の確かめ:

- `sh plan/ws334/tests/storage-test.sh plain`・`asan` → `test-storage: PASS (92 checks)`、`storage-test: PASS`。中身: files の backend の instance に response の cookie（Max-Age・Expires・両方（Max-Age が勝つ）・session・Max-Age=1・過去の Expires・persistent を session で置き換え）と script の cookie（`net_cookie_store_script`、host の `python3 -m http.server` の頁の document.cookie）を入れ、destroy → 作り直しで期限付きだけが戻る、Max-Age=1 は 1.3 s 後に戻らず record も消える、session に置き換えた物の record も消える（record の file の数 6 → 5）。頁の localStorage が作り直しで戻る、今の v1 の `.ls`（file: の origin、`local-storage/66696c653a2f2f.ls`）が読めて次の変更で v2 になる、2 つの instance（a・b）が cookie と localStorage を分ける、使用中の directory の 2 つ目の `browser_storage_files_open` は EBUSY、backend 無しの instance も動く、版の違う options・版の違う表・member の欠けた表は ENOTSUP で close が 1 回、view の残る instance の destroy は EBUSY、fake の write の EIO で view の console に "storage: could not save (…)" が 1 回だけ（process を 2 回）、instance 無しの view は XDG_DATA_HOME を置いても何も書かない。変異（cookie の読み込みで全部を捨てる・LOCAL の書きを止める）を入れると 12 件 FAIL を確かめた（戻して PASS）。
- `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`。libbrowser.a（net/cookie.c・page/storage.c・storage/instance.c を含む全部）が x86_64・aarch64 とも warning 0（-Werror）。libbrowser.a の import は win64 165・winarm64 161 で全部が表の中（新しく要ったのは UCRT の `strcpy` だけ。表に足した）。3c: 公開の header の 51 の関数が libbrowser.a に定義され、exports.map の 7 つの名前・glob が全部当たる。
- `sh plan/ws334/tests/fetch-wine.sh` → `fetch-wine: PASS`（test-program の source から net/cookie.c を外した。cookie.c は libbrowser.a に入った）。
- `LC_ALL=C.UTF-8 timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`（34 本: posix 16・win32 18）。
- `sh plan/ws334/tests/net-ring-test.sh plain`・`tsan` → PASS（net/*.c に storage/queue.c・records.c を足した）。`media-wake-test.sh plain` → PASS。
- zedBSD amd64（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=<写し> -o <写し>/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libbrowser.so build/amd64/bin/browser`、写しは main の `build/amd64/sysroot` を `build/tmp/p023/sysroot-amd64` に写し tree の `include/libc`・`userland/desktop/include` を重ねた物）→ exit 0、warning 0。
- `sh plan/ws334/tests/check-skeleton.sh` は FAIL（`libkeiland/platform/platform.h tests the system`）だが基点 15cd956cf でも同じ FAIL（この Phase と無関係。p003・p016 の物）。
- `git diff --check` → 出力なし。

自分で決めた点・phase.md と code の食い違い:

- **cookie の key**（手順 4）: 開く時に COOKIES の space の記録を全部 jar に読む（期限切れ・壊れた物・重複・512 を超えた物は remove）ので、disk の記録は全部 jar にある。新しい cookie の key は jar の中の key の一覧から（衝突の番号 0〜15 の最初の空き）決め、queue の read を待たない（lock の外で読む手順が要らない。network の thread も待たない）。16 個とも埋まった cookie は memory だけ。
- **localStorage の key**（手順 4）: 領域を初めて使う時に page の thread が衝突の番号 0〜15 を順に read する。無い key・自分の origin の v2 の key・collision 0 の短い origin の hex の key にある v1 の記録を自分の物にする。読めない・decode できない記録は自分の失われた物として remove し、その key を取る（中身は空）。
- **page_fetch・page_fetch_response に jar の引数**（phase.md の手順 5 は link.c の 2 行だけだが、2 つの関数は page を受けないので、`struct net_cookie_jar *jar` を最初の引数に足し、呼び手 12 か所（page.c・images.c・module.c×2・media.c・frame-load.c・sheets.c・fonts.c・script.c×4）が `page_cookies(page)` を渡す）。WS074 の試験 3 本（host-data-url・host-file-subresource・host-resource、data: と file: だけ）は NULL を渡す。`net_http_fetch`・`_send`・`net_http_request_text*`・`net_http_parse_response` も jar を最初の引数に。
- **loader の jar**: `net_loader_create` の引数は変えず（WS074 の host-loader.c・host-frame-load.c、WS334 の net-ring-test.c が直に呼ぶため）、`net_loader_set_cookies(loader, jar)` を足し `page_net_create(jar, …)` が作った直後に呼ぶ。jar の無い loader（それらの試験）は cookie を送らず保たない。
- **view の無い page**（WS074 の host 試験 22 本が `page_create` を直に呼ぶ）: `page_instance(page)` は page に instance が無ければその thread の既定の instance を使う（page に覚える）。
- **既定の instance**: phase.md の通り thread ごと（`pthread_key_t`、thread の終わりに view が無ければ destroy、あれば process の終わりまで）。違う thread の instance NULL の view の間では cookie と Web Storage を共有しなくなった（前は process に 1 つ）。`browser_instance_destroy` は既定の instance に EINVAL。
- **font の companion**: `struct browser_fonts` に `bold`・`fallback_mono`。中の `struct text_font_paths` に field を足すと、memset せず 3 つだけ入れる WS074 の host 試験（17 本）が壊れるので、別の `struct text_font_companions` と `text_system_open_with`・`page_set_font_companions` にした（`text_system_open` は既定の companion で今と同じ）。既定の path は `text_default_path(TEXT_PATH_BOLD/FALLBACK_MONO)` = data の directory + "/fonts/keiland-bold.ttf"・"/fonts/keiland-fallback-mono.ttf"（zedBSD は今の `KEILAND_FONT_*` と同じ path）。公開の `browser_fonts` の field が増えたので、memset していない利用側（host-view・host-form・host-sticky・client.c）に memset を 1 行足した。
- **Expires**: 前は Expires を読まず Max-Age の 0 以下だけが削除だった。今は RFC 6265 §5.1.1 の cookie-date を読む（過去の Expires の cookie は削除・保存しない。これは挙動の変化）。期限の上限は 9999-12-31（Max-Age の大きな値はそこで止める）。400 日の上限（RFC 6265bis・Chrome）は入れていない。
- **created**: 置き換えた cookie の作った時を引き継ぐ（RFC 6265 §5.3 の 11.3）。jar の順は作り直しの時に created の順に並べ直す。`used` は memory だけ（送る・読むたびに書かない）。SameSite は読まず record は UNSET（読み込んだ record の SameSite の bit はそのまま保って書き戻す）。
- **誤りの知らせ**: view の console の level 1（warn）に "storage: could not save (<strerror>)"、view ごとに 1 回。`wb_storage_queue_take_error` は queue に 1 つなので、同じ instance の別の view が先に取ればそちらで知らせる。
- **shell**: `$XDG_DATA_HOME/keiland/browser`（無ければ `$HOME/.local/share/keiland/browser`）の files の backend。open の失敗（EBUSY を含む）と home が無い時は backend 無しで続け stderr に 1 行。instance は shell_release で view の後に destroy。
- **headless の変化**: `--dump`・`--run`・`--js`・`--render` は instance 無し（既定の instance）なので localStorage の file を書かなくなった（前は headless でも `$XDG_DATA_HOME/keiland/browser/local-storage/` に書いた）。WS074 の host 試験の `setenv("XDG_DATA_HOME")` は効かなくなったが消していない（P8 の判断、Q1 経由）。
- **cross-build.sh**: libbrowser.dll・.def が無くなった（p036）ので、phase.md の「`.def` は exports.map の glob を展開」の代わりに 3c（公開の header の関数の定義と exports.map の名前の照合）を足した。3b は libbrowser.a の全部（`net_cookie_`・`page_storage_` の除外を外した。`media_` の除外は libmedia の Windows の port まで残す）。
- `libbrowser/Makefile` の header の一覧に `storage.h` を足した（standalone の install 用。browser.h が `<browser/storage.h>` を include するため）。

未実施:

- zedBSD の guest の browser の確かめ（localStorage が再起動で残る、design §7.1 段 5、`plan/ws074/tests/browser-guest.sh`）は Q1 の割当で。shell の window の mode の files の backend は host で通していない（host-shell.c が shell の代わりのため）。
- `run-test262-parse.py`（suite の取得が要る）、Linux の Keiland・FreeBSD・zedBSD arm64 の build、本物の Windows・aarch64 の実行、TSan での instance・jar の試験（net-ring-test の tsan は jar を NULL で通るだけ）。

Q1 への問い・判断の材料:

1. WS074 の host 試験の `setenv("XDG_DATA_HOME")`（host-position・host-link・host-svg-text・host-svg-length・host-relayout・host-view・host-form・host-sticky、run-http-tests.py の `os.environ`）は効かなくなった。消すかは P8 の判断（phase.md の通り）。
2. 過去の Expires を持つ cookie の削除と、Expires が未来の cookie の永続は挙動の変化（前は Expires を読まなかった）。WS074 の試験の件数は変わらない。

消してよい物（Q1 へ）: `build/tmp/p023-base`・`build/tmp/p023-tip`（試しの tree の展開と stage1 の出力）、`build/tmp/p023-mut`（変異の元の写し）、`build/tmp/p023`（sysroot の写しと log）、`build/tmp/p023-*.log`・`build/tmp/p023-stage1.sh`、`build/tmp/ws334-storage.run.*`（最後の run 以外）、`build/amd64`（zedBSD の試しの build の出力）。どれも使い捨て。
