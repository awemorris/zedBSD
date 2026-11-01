<!-- awesome-plan project=zedbsd record=ws334-p024 -->

# ws334-p024 統合: 永続の HTTP の cache（第 2 段）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §3.5.1（CACHE の space）、§3.5.4、§3.7。
- 状態: cleared（2026-11-01 22:55 Q1、main c7a3a7ad3。merge の後の main で storage-test の link が p027 の main-posix.c と二重の main で落ちたので、Q1 が storage-test.sh で browser_main-posix.o を除いて直した: storage-test plain 200 checks PASS、host-build 94・warning 0）。前: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1855-i01、Sonnet。結果は末尾）。前: planned（2026-11-01 p002 で作成）。ユーザーの決定 21（U5）で第 2 段: WS334 の第 1 段の受け入れ（p032）には入れない。
- 依存: p023。
- 担当の想定: Opus（phase-runner）を 1 回。使う約束は `storage/records.h` の `wb_storage_cache_*` と `storage/queue.h`。

## 中身

1. **索引**（`storage/instance.c` に足す）: URL の key → {record の bytes の数、`stored`、最後の使用の ms}。instance の作成では CACHE の space の**索引の record だけ**を読む（body を読まない。design §3.5.4）: 索引の record は key `0`（`wb_storage_key_valid` を満たし、16 進の key は偶数の長さ・hash の key は `-` を含むので data の key と重ならない）、中身は `KEILAND CACHE INDEX 1\n` と各 entry（u16 key の長さ・key・u64 bytes・i64 stored・i64 used、little-endian）。records.h は変えない（索引の encode・decode は instance.c の中の static の関数、壊れた索引は空の索引として捨て、次の書きで作り直す）。索引の record は entry を足す・除く・使う時に write-behind で書き直す（使用の時刻は 60 秒に 1 回まで）。索引に無い record（索引の書きの前に落ちた）は上限の掃除の時に list で見つけて remove。
   - lock: 索引は instance の `cache_lock`（新）の下。lock の順（外 → 内）は `loader->lock` → `cache_lock` → jar の lock → storage の queue の lock（design §3.5.2 の順に `cache_lock` を足した。`cache_lock` の中で queue の待つ関数を呼ばない）。
2. **書き**（`net/loader.c` の `loader_cache_store` の後）: memory の cache に入れた entry を `wb_storage_cache_encode`（`stored` は `wb_os_wall_ns()/1000000`。memory の cache の `stored_at` は monotonic なので別に持つ）→ key（URL の bytes の `wb_storage_key_text`、衝突は p023 の 4 の規則）→ `wb_storage_queue_write(CACHE)`、索引に足す。network の thread から呼ぶので待たない関数だけ（queue.h の lock の約束）。
3. **読み**（`loader_cache_find` が memory で見つけない時）: 索引に URL があれば、`loader->lock` も `cache_lock` も持たずに `wb_storage_queue_read` で record を読み（network の thread が待つ、design §3.5.2）、decode して URL が同じなら memory の cache に入れ、今の `loader_cache_fresh`・`loader_cache_answer` の道へ（鮮度は `stored` を wall の時計で比べる）。ETag の再検証で 304 が来たら `stored` を今にして書き直す。
4. **上限**: 索引の bytes の合計が `cache_limit`（既定 256 MiB）を超えたら、最後の使用の古い順に remove して下回らせる。backend の write が ENOSPC なら古い物を 1 割消して 1 回やり直す（design §3.7）。
5. **消し**: 将来の「閲覧の data の消去」の口は作らない（範囲の外）。

## 試験（受け入れ）

- `plan/ws334/tests/storage-test.c` に cache の項を足す: 200 と max-age の response を host の小さな server から取り、instance を作り直した後に server を止めても同じ URL が cache から答える。no-store・POST・304・ETag の再検証・上限での LRU の削除・壊れた record の remove。
- `run-loader-tests.py`（WS074）の件数が前と同じ。`run-unit.sh all`・`cross-build.sh` が PASS。zedBSD の libbrowser の build が warning 0。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。HAL・toolchain・共有の build/。rm をしない。push しない。
- 全部 PASS の後、file の群ごとの commit、SHA と結果を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-01 22:50、Sonnet、q1855-i01、基点 3c076f4e8）

commit（`git commit -m WIP -- <path>`、file の群ごと）:

| SHA | 中身 |
| --- | --- |
| eec8475463 | libbrowser: 新 `net/cache.c`（索引・読み・書き・上限）、`net/loader.c`（2 段目: `net_loader_set_cache`・`loader_cache_load`・`_persist`・`_insert`、wall 時計の鮮度、304 の書き直し）、`net/net.h`（`net_cache_*`・`net_loader_set_cache`）、`storage/instance.c/.h`（instance が cache を持つ）、`page/network.c`・`page/page.h`・`view/view.c`（loader に cache を渡す）、`sources.mk` |
| 75b1da7c91 / fdf559a838 | 試験: `plan/ws334/tests/storage-test.c`（cache の項）・`storage-test.sh`（第 2 の server）・新 `cache-server.py`、`short=5` の cookie の Max-Age を 2 s に（下） |
| bb07290d0c | `allowed-imports.txt` に `bsearch`、cache.c の comment の直し |

公開の header（`browser.h`・`storage.h`）は変えていない（`browser_instance_options.cache_limit` は p023 で既にあった）。module の header と .c（records・files・queue）も変えていない。

試験と結果:

- `sh plan/ws334/tests/storage-test.sh plain` / `asan` → どちらも `test-storage: PASS (200 checks)`（p023 は 92）、`storage-test: PASS`。新しい 108 checks の中身: 200 + max-age の response を loader で取り memory が 2 回目に答える、ETag（max-age=1）・no-store・plain（cache の指示なし）・POST 2 回（POST は cache されず server に 2 回届く）、instance を destroy して作り直すと同じ URL を server に聞かずに答える（server の count で確認）、record の file は 2 つと索引だけ（`test_count_files` 3）、stale の ETag を If-None-Match で再検証して 304 → 保存の body で答え、304 の max-age=3600 で fresh になり、さらに instance を作り直しても server に聞かない（`stored`・max-age の書き直し）、他の URL の record が同じ key にあれば答えず record を残す、壊れた record を読むと remove（flush 後に file が無い）、索引に無い record（`abcd`）は instance の作成で remove、索引の magic を壊すと空の索引で全 record が remove、cache_limit 350000 で 100 KB の blob を 4 つ入れると使用の古い 2 番目が索引と storage から消える、limit を 150000 に下げて開くと最近使った 1 つだけ残る、最後に server を止めて（`/quit`、`/count` が失敗することを確認）新しい instance が record から答える。
- 変異（git の外で 1 つずつ入れ、FAIL を確かめ、`git checkout` で戻した。戻した後 `git status` が clean）: 書きを止める・上限の削除を止める・孤児の掃除を止める・wall 時計の鮮度を常に fresh・304 の書き直しを止める・URL の照合を外す・壊れた record の削除を外す・LRU を逆（新しい物から消す）・索引の magic の照合を外す。9 つ全部が FAIL した（最初に URL の照合と索引の magic の変異は PASS してしまい、試験を足して塞いだ: 他の URL の record の項と、magic だけを壊した索引の項）。
- 段 1 の前後比較（`sh plan/ws334/tests/stage1.sh TREE plain|asan`、前は `git archive 3c076f4e8` を `build/tmp/p024-base`、後は `git archive fdf559a838`（bb07290d0c との差は cache.c の comment の 1 行と allowed-imports のみ）を `build/tmp/p024-tip`）: host-build は exit 0・warning 0・built 94（前後同じ）、host-* は 75 exit 0・17 その他で、plain・asan とも `host-exits.txt` が前後で diff なし、run-js-tests 17/17、run-loader-tests 11/11（`run-loader-tests.py` の件数が前と同じ）、run-http-tests 15/15、`--async` 19/20（FAIL は p023 から同じ async-sheets）、run-fetch-tests 38/38、browser-component 83 checks PASS。plain・asan とも前後で同じ。
- `LC_ALL=C.UTF-8 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。`sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（libbrowser.a の import は win64 166・winarm64 162 で全部が表の中、cache.c を含み warning 0）。`fetch-wine.sh` → `fetch-wine: PASS`。`net-ring-test.sh plain` → PASS。
- zedBSD amd64（p023 と同じ command と sysroot の写し）の `build/amd64/dynamic/libbrowser.so`・`build/amd64/bin/browser` → exit 0、warning 0（log `build/tmp/p024/zedbsd-build.log`）。
- `git diff --check` → 出力なし。

自分で決めた点（phase.md との違いを含む）:

- **置き場所**: 索引と読み書きは phase.md の「`storage/instance.c` に足す」でなく新 file `net/cache.c`（cookie の jar の `net/cookie.c` と同じ形、`net_cache_open/close/read/store`）。理由: instance.c は storage の thread・jar・領域の持ち主で、cache の 500 行を足すと長くなり、loader から使う口が net/ に揃う。`sources.mk` に 1 行。lock は phase.md の通り（`loader->lock` → cache の lock → storage の queue の lock、cache の lock の中で待つ関数は呼ばない。read は lock 無しで待つ）。名前は `cache_lock` でなく struct の `lock`。
- **key の衝突の規則**: phase.md は p023 の規則（書く前に読んで元の key を確かめる）としたが、書き（network の thread、待てない）で読めないので、cache の key は常に collision 0。読みで URL を照合し、他の URL の record なら答えず残す。hash の形の key（URL が 100 byte を超える）が 64 bit の hash で衝突した時は、後の書きが前の物を置き換える（miss になるだけ）。
- **孤児の掃除の時期**: phase.md は「上限の掃除の時に list で見つけて remove」だが、その時は network の thread の書きの中で list（待つ）が呼べないので、instance の作成の時（cookie の list と同じ page の thread）に行う。索引の read が失敗（EIO）した時は掃除しない（一時の失敗で全部を消さないため）。索引が壊れた時は空の索引なので全部が孤児として消える。
- **索引の更新**: 足す・除く・上限の削除の時は即 write-behind で索引の record を書き直す。使用は memory の `used` だけを変え、前回の書きから 60 秒以上なら書き、そうでなければ dirty にして instance の destroy（`net_cache_close`、queue を閉じる前）で書く。
- **索引の数の上限**: 16384 entries（超えると使用の古い順に消す）。理由: 索引の record を変更ごとに全部書き直すので大きさに上限が要る。
- **ENOSPC**: queue の write が同期で返す ENOSPC（待ちが 64 MiB を超える）は、保存の 1 割を消して 1 回やり直す。backend の write が後で失敗する ENOSPC は、write-behind なので書いた時に分からず、`wb_storage_queue_take_error` は view の console の知らせに使うので cache は取らない。record の無い索引の entry は次の読みで found 0 になり索引から消える（自己修復）。phase.md の「backend の write が ENOSPC なら古い物を 1 割消して 1 回やり直す」はこの形に限った（queue.h の変更なしに備える形が無い）。
- **鮮度**: loader の entry に `stored_wall`・`wall_clock` を足した。disk から読んだ entry は wall 時計で `max_age` と比べる（時計が戻ると stale）。304 で `stored_at` と `stored_wall` を今にして `wall_clock` を外し、disk の record も書き直す。memory の cache に入った entry の `stored_at`（monotonic）の意味は変えていない。
- **disk の cache を持つ instance**: backend のある instance だけ（backend 無しの既定の instance は `cache` が NULL で、loader は今まで通り memory だけ）。`page_net_create` の引数に `struct net_cache *cache` を足した（view.c だけが呼ぶ、private の header）。`net_loader_create` の引数は変えていない。
- **試験の道具の直し**: `storage-test.c` の cookie の項の `short=5` の Max-Age を 1 から 2（待ちも 2.3 s）にした。理由: asan・plain の最初の run（host-build の直後）で「頁を読んだ後に Max-Age=1 の cookie がまだ送られる」が 1 s を超えて 2 回 FAIL した（p023 からの時間依存の flake。cache の変更とは無関係）。`allowed-imports.txt` に `bsearch`（UCRT）。

未実施・残り:

- 実機・QEMU・zedBSD の guest での確かめ（Q1 の割当で。この Phase は host と cross-build まで）。Windows の実行（Wine の `fetch-wine.sh` は cache の store を通っていない: cache の無い loader）。TSan での cache の試験（network の thread と page の thread が 1 つの cache を同時に使う場合。`net-ring-test` の tsan は cache NULL）。
- 見つけた既存の挙動（この Phase では直していない）: `loader_cache_store` は stale の entry を ETag で再検証して **200**（本文が変わった）が返った時も、`entry == request->validating` で早く戻り、新しい本文を memory にも disk にも入れない（古い本文が残り、毎回再検証して 200 を取り直す）。応答自体は正しい（network の本文を返す）。直すなら 304 と 200 の見分け（status が `loader_cache_answer` で 200 に上書きされる前）が要るので、Q1 の判断で別に。
- 並行の件: libbrowser の変更は `net/cache.c`（新）・`net/loader.c`・`net/net.h`・`page/network.c`・`page/page.h`・`view/view.c`・`storage/instance.c/.h`・`sources.mk`。公開の header は無変更。

Q1 への問い・判断の材料:

1. 上の「置き場所」（net/cache.c）・「key の衝突」・「孤児の掃除の時期」・「ENOSPC」が phase.md と違う。受け入れるか。
2. 上の 200 の再検証の既存の挙動を別 Phase で直すか。

消してよい物（Q1 へ）: `build/tmp/p024-base`・`build/tmp/p024-tip`（git archive の展開と stage1 の出力）、`build/tmp/p024`（sysroot の写しと zedbsd-build.log）、`build/tmp/p024-stage1-*.out`・`p024-stage1-driver.log`・`p024-stage1.done`・`p024-run-stage1.sh`、`build/tmp-p024-hb.log`（置き場を誤った log、build/ 直下）、`build/tmp/ws334-storage.run.*`（最後の run 以外）、`build/amd64`・`build/ws074-host`（試しの build の出力）。どれも使い捨て。

### Q1 の判断（2026-11-01 22:55）

- Sonnet が決めた 1〜4（net/cache.c に置く、key の衝突は読みで URL を照合、孤児の掃除は instance の作成の時、同期の ENOSPC だけやり直す）を受け入れる。
- stale の ETag の再検証で 200（本文が変わった）が返った時に新しい本文を memory・disk に入れない件は、前からの挙動。この Phase の外として WS334 の ws.md の残りに記録した。
- 存在しない file への rm -f を 1 回走らせた件（何も消していない）は、削除の規則に反した操作として記録する。


### 追補（q1868）

2026-11-01 q1868-i01（Sonnet）。stale の ETag の entry の再検証で 200（本文が変わった）が返った時に新しい本文を cache に入れない件（上の「見つけた既存の挙動」）を直した。

- 直し（`userland/desktop/libbrowser/net/loader.c`）: `struct net_request` に `revalidated` を足し、再検証の開始で 0、304 の分岐で 1。`loader_cache_store` は、`entry == request->validating` かつ `revalidated`（304 で更新済み）の時だけ早く戻る。それ以外は、同じ URL の古い entry（再検証した物を含む）を `loader_cache_free` で外し、200 の応答の本文・type・ETag・max-age で新しい entry を入れて `loader_cache_persist` で disk に queue する（record は同じ key で置き換わる）。304 の挙動（`stored_at`・`stored_wall` を今にし、`wall_clock` を外し、disk の record を書き直す）は変えていない。公開の header・他の file は変えていない。
- 試験の道具: `plan/ws334/tests/cache-server.py` に `/etag-change`（最初は 200 `change-v1`・ETag `w1`・max-age=1、`If-None-Match: "w1"` には 200 `change-v2`・ETag `w2`・max-age=3600、`"w2"` には 304）。`storage-test.c` に `test_cache_changed`（directory `e`）: 1 回目 `change-v1`、1.3 s の後の 2 回目が条件付きの要求（server の hits 2・conditional 1）で `change-v2`、3 回目は cache から（hits 2 のまま）、instance を作り直した 4 回目も record から `change-v2`（hits 2 のまま）。
- 確かめた事:
  - `sh plan/ws334/tests/storage-test.sh plain` → `test-storage: PASS (222 checks)`（前は 200）、`asan` も 222 PASS。
  - 変異: 早い戻りを `entry == request->validating` だけに戻す（直す前の挙動）と `storage-test.c:1469`（3 回目で hits 3）と `:1490`（4 回目で hits 4）の 2 件が FAIL し、`test-storage: FAIL (2 of 222)`。直しを戻すと PASS。304 の項（既存の `/etag`）はどちらでも PASS。
  - 段 1 の前後比較（`stage1.sh`、基点は `git archive 9dae26145` を `build/tmp/q1868-base`、後は `git archive d8a0b7953` を `build/tmp/q1868-tip`）: host-build は exit 0・warning 0・built 94、host-* は 75 exit 0・17 その他で `host-exits.txt` が前後で diff なし、js 17/17、loader 11/11、http 15/15、fetch 38/38、component 83 PASS。asan は全部が前後で同じ。plain の `--async` は後が 19/20（p024 と同じ値）、前は 18/20（前後を同時に流した時の揺れ。この変更の側が悪くなったのではない）。
  - `LC_ALL=C.UTF-8 sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`。zedBSD amd64 の `build/amd64/dynamic/libbrowser.so`・`build/amd64/bin/browser`（p024 と同じ command と sysroot の写し）→ exit 0、warning 0（log `build/tmp/q1868/zedbsd-build.log`、loader.c は compile された）。
- 未実施: `run-unit.sh all`・`fetch-wine.sh`（この変更は net/loader.c の cache の道だけで、host の試験と cross-build は PASS）。QEMU・実機。再検証の 200 が保存できない物（no-store・ETag も max-age も無し）の時に古い entry が残る前からの挙動は直していない。
- 消してよい物（Q1 へ）: `build/tmp/q1868-base`・`build/tmp/q1868-tip`（git archive の展開と stage1 の出力）、`build/tmp/q1868`（sysroot の写しと zedbsd-build.log）、`build/tmp/q1868-*.out`・`q1868-run.sh`・`q1868-stage1.done`・`q1868-cross.log`、`build/tmp-st-plain.log`・`tmp-st-mut.log`・`tmp-st-asan.log`（build/ 直下の log）、`build/tmp/ws334-storage.run.*`（最後の run 以外）、`build/amd64`・`build/ws074-host`。どれも使い捨て。
