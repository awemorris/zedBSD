<!-- awesome-plan project=zedbsd record=ws345-p002 -->

# ws345-p002 shell が登録する thread の表

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P8 q1968-i01、branch `agent/p8-ws345`、基点 51bd601bf4）

## 目的

libbrowser が自分で thread を作らない形にする（ユーザーの決定、design.md §5.1）。libbrowser の thread・lock・condition を全部、shell が登録する表を通す。動作は変えない。

## file

- `userland/desktop/include/browser/browser.h`: `struct browser_threads`（`BROWSER_THREADS_VERSION` 1、`is_current` を含む、design.md §5.1）、`browser_threads_set`、`BROWSER_API_VERSION` を 5 に。`exports.map`。
- `userland/desktop/libbrowser/base/thread.c`・`thread.h`（新）: 表の包み（`wb_thread_start`・`wb_thread_join`・`wb_mutex_*`・`wb_cond_*`）、`base/once.c`（C11 atomic の一度）。
- 替える所: `net/loader.c`・`net/cache.c`・`net/cookie.c`・`net/tls.c`（`tls_reason_key` を接続・request の context に）・`net/url.c`・`storage/queue.c`・`storage/instance.c`・`text/font.c`・`base/os-file-posix.c`・`base/os-system-posix.c`（`wb_os_thread_name` を無くす）、`page/media.c`（libmedia の engine を開く時に表の `start`・`join` を渡す）、`userland/desktop/libmedia/engine.c`・`media.h`（engine の thread を渡された口で作る。渡されなければ今の pthread、videoplayer 用）。
- shell: `userland/desktop/browser/shell/threads.c`（新、kl_thread・kl_mutex・kl_cond の表）、`browser/main.c`（全ての mode の始めに `browser_threads_set`）、`browser/sources.mk`。
- 試験の側: POSIX の表の実装（`userland/tests/browser-threads/threads-posix.c` 新）を、view・instance を作る試験と道具が使う: `plan/ws074/tests/host-*.c`・`wpt-ready-render.c`・`host-build.sh`、`plan/ws334/tests/scheme-test.c`・`storage-test.c`（と WS334 の Windows の試験の harness が要れば）、`plan/ws345/tests/stall-probe.c`、`userland/tests/browser-probe/main.c`、`plan/tools/browser-component/client.c`（plan/tools は Q1 の物、Queue で許可を確かめる）。WS074・WS334 の試験の変更は Q1 が許可した（ws.md の Q1 の判断）。変えた試験の file は全部「結果」に書く。
- `plan/ws074/tests/host-build.sh` に ThreadSanitizer の variant（`tsan`）を足す。保守的な stack の走査（`vm/heap.c` の `heap_scan_stack`）に `no_sanitize("thread")`。
- Windows: `apps/browser-win32/Makefile`（libbrowser の winpthreads への依存を外す。libmedia の lock が使う間は link に残る）。

## 手順

1. 表と包みを作り、`browser_threads_set` の契約（一度、最初に、欠けは `ENOTSUP`、二度目は `EBUSY`）を実装する。
2. libbrowser の `pthread_*` を包みに替える。`pthread_once` は atomic の一度、`pthread_key`（thread の既定の instance）は process の既定の instance に。
3. thread の作成の失敗の扱い（design.md §5.1 の契約）: 作成の途中まで始めた thread を止めて join。
4. shell と試験の実装、`main.c` での登録。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-threads-table.c`: 表の無い instance の作成が `ENOTSUP`、二度目の set が `EBUSY`、`start` が 2 回目で失敗する表で view の作成が `EAGAIN` で失敗し、始めた thread が全部 join される（表の側で数える）、名前の一覧（`browser-net`・`browser-resolve`・`browser-storage`）。
- `nm` で libbrowser の object に `pthread_create` が無い事（libmedia を除く）。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。
- Windows: `make browser-win64` と Wine で OmniGrammaton が起動して start の頁を出す（plan/ws334 の Wine の道）。

## 受け入れ

- libbrowser（libmedia の lock を除く）が `pthread_*` を呼ばない。thread は全部 shell の表で作られ、名前は libbrowser の物。
- 上の試験と回帰が前と同じ。

## 結果（2026-11-02 P8、q1968-i01）

- 実装: `<browser/browser.h>` を版 5 にし `struct browser_threads`（`BROWSER_THREADS_VERSION` 1、start・join・is_current・mutex 4・condition 6）と `browser_threads_set` を足した。`base/thread.c`（表の写しと包み）・`base/once.c`（atomic の一度と、初めて取る時に作る static の mutex）。libbrowser の `pthread_*` を全部替えた（loader・storage queue・cache・cookie・tls・url・font・instance・os-file-posix）。`wb_os_thread_name` は無くし、名前は表の `start` が付ける。thread ごとの値は無くした: TLS の失敗の理由は接続（`net_tls_reason`）・request（`net_request_reason`）・response（`net_response.reason`）・頁（`page_failure_reason(page)`）で運ぶ。thread の既定の instance は process の既定の instance に。libmedia の engine は `media_engine_open` の新しい引数 `struct media_threads` で thread を作り、libbrowser は `browser-media` の名で表を渡す（libmedia の lock は pthread のまま、J3）。shell は `shell/threads.c`（kl_thread・kl_mutex・kl_cond、三つの platform に共通）、`main.c` が全ての mode の最初に登録する。試験と道具は `userland/tests/browser-threads/threads-posix.c`（POSIX threads、Windows では winpthreads）。今ある thread の stack は system の既定（0）のまま。
- 変えた試験・道具の file（Q1 の許可、ws.md の Q1 の判断）: `plan/ws074/tests/host-build.sh`（`tsan` の variant、threads-posix の link、`wpt-ready-render` も作る）、`plan/ws074/tests/host-shell.c`（`shell_threads`）、`plan/ws074/tests/host-threads.c`（新、unit の試験に main の前に表を渡す）、`plan/ws074/tests/wpt-ready-render.c`、`plan/ws074/ws.md`（`wpt-ready-render` の手の build の行、Q1 の許可）、`plan/tools/browser-component/client.c`・`run.sh`、`plan/ws334/tests/modules.sh`・`run-unit.sh`・`unit/test-queue.c`・`unit/test-os-file.c`・`unit/test-files.c`・`unit/test-os-system.c`（名前の試験を外した）・`tls-main.c`・`fetch-main.c`・`protect-main.c`・`net-ring-test.c`・`net-ring-test.sh`（前から link できなかった `tls-static.c` を外し、`-Iuserland/desktop/include`）、`userland/tests/browser-probe/main.c`・`Makefile`、`plan/ws345/tests/stall-probe.c`・`.sh`。
- 確かめ（host）:
  - `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan`: exit 0、warning 0。
  - `sh plan/ws345/tests/host-threads-table.sh plain`・`tsan`: 16 checks、0 failed（4 本 start、4 本 join、名前 browser-storage・browser-resolve・browser-net、表の無い時の ENOTSUP、EINVAL・ENOTSUP・EBUSY、2 本目の start の失敗で view が EAGAIN で 1 本目が join される）。
  - `nm -u`: engine の object（libmedia・compat library を除く）に `pthread_` の参照が無い。
  - 段 1（`sh plan/ws334/tests/stage1.sh TREE plain`、前 `git archive 51bd601bf4`、後 `git archive 4b87b68ad1`、build/tmp/p8s2）: host-build warning 0（101 → 102 本、増えたのは wpt-ready-render）、host-* 82 exit 0・17 other で試験ごとに同じ、js 19/19、loader 11/11、fetch 38/38、component 83 PASS、http-async 19/20（前後とも async-sheets の FAIL）。http は後で 13/15（untrusted・wrong-name の TLS の理由が消えた: `page_fetch_response` が失敗の時に response を作り直していた）→ 344c486700 で直して 15/15。
  - ThreadSanitizer: `host-threads-table` 16/16、`run-loader-tests.py` で tsan の host-loader 11/11、host-back-forward・host-script-navigation exit 0、`net-ring-test.sh tsan` PASS、どれも ThreadSanitizer の報告 0。host-navigation-wait は tsan で「1 回の呼び出し 400 ms 以下」の時間の check だけ FAIL（735 ms、TSan の遅さ。plain は PASS）。
  - WS334 の unit（`run-unit.sh --posix queue os-file os-system files`）: PASS。`net-ring-test.sh plain`: PASS。
  - zedBSD の build（`make -j32 ZEDBSD_CONFIG=plan/ws074/tests/config-amd64-browser.mk BUILD=build/ws345-p002 build/ws345-p002/bin/browser build/ws345-p002/bin/browser-probe`）: exit 0、warning 0。
  - Windows: `sh plan/ws334/tests/cross-build.sh`（344c486700）: PASS（win64・winarm64）。`sh plan/ws334/tests/wine-run.sh`（be84456282）: PASS（21 passed、0 failed、6 は本物の Windows の項目で not run）。`tls-wine.sh`: PASS（blocking と loader とも 200）、`fetch-wine.sh`: PASS（26b81b83d9）。
- 未実施: asan の variant の段 1、QEMU の zedBSD の起動（この Phase は動きを変えない）、本物の Windows、test262。
