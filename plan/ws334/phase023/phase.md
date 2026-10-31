<!-- awesome-plan project=zedbsd record=ws334-p023 -->

# ws334-p023 統合: instance と API v4（cookie の jar・Web Storage を instance へ、storage の module をつなぐ）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §3.5〜§3.8、§2.5、§9.1・§9.2。
- 状態: planned（2026-11-01 p002 で作成）。
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

1. **instance**（新 `storage/instance.h`・`storage/instance.c`、private）: `struct browser_instance` = cookie の jar（下の 2）、localStorage と sessionStorage の領域の一覧（今の `page/storage.c` の static の `storage_areas` をここへ）、`struct wb_storage_queue *queue`、cache_limit、view の数（`view_count`、page の thread だけが触る）。create: options を確かめ、`wb_storage_queue_open`（失敗なら storage の close を呼んで返す）、jar を作り、COOKIES の space を `wb_storage_queue_list` → 各 key を `wb_storage_queue_read` → `wb_storage_cookie_decode` → 期限（`expires` ≤ `wb_os_wall_ns()/1000000`）の過ぎた物と decode の失敗は捨てて `wb_storage_queue_remove`、残りを jar に。destroy: view_count が 0 でなければ EBUSY、`wb_storage_queue_close`、jar と領域を放す。既定の instance は `pthread_once` で backend 無しで作る。
2. **cookie の jar**（`net/cookie.c`・`net/net.h`）: static の `cookie_jar`・`cookie_count`・`cookie_lock` を `struct net_cookie_jar`（配列 COOKIE_MAX = 512、数、mutex、queue の pointer）に移す。`struct cookie` に `int64_t expires`（ms、session の cookie は -1）・`created`・`used`、`same_site`。Expires・Max-Age を保つ（今は過去の削除だけ）: Max-Age が優先（RFC 6265 §5.3 の 3）、`expires` = 今 + Max-Age 秒。期限の過ぎた cookie は header・string を作る時に捨てる。`net_cookie_store`・`_header`・`_store_script`・`_string` の最初の引数に `struct net_cookie_jar *jar`。期限のある cookie を足す・替える時は `wb_storage_cookie_encode` → key（`wb_storage_cookie_original_key` → `wb_storage_key_text`、collision の扱いは下の 4）→ `wb_storage_queue_write(COOKIES)`、消す時は `wb_storage_queue_remove`。queue の関数は待たない物だけを jar の lock の中で呼んでよい（queue.h の lock の約束）。session の cookie は保存しない。512 を超えて古い物を捨てる時、それが保存した物なら remove。
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
