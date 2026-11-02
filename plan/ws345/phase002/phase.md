<!-- awesome-plan project=zedbsd record=ws345-p002 -->

# ws345-p002 shell が登録する thread の表

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

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
