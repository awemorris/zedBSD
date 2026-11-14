<!-- awesome-plan project=zedbsd record=ws334-p035 -->

# ws334-p035 規約の全文の見直しと header の持ち越しの直し

- 親: [WS334](../ws.md)。AGENTS.md §2（code を作る WS の最後の Phase）、手順 docs/agent/spec-module-delegation.md の §4.6（header の持ち越し）。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p033・p034・p036。
- 担当の想定: conformance-reviewer（書き方だけ、動作を変えない）。

## 中身

- WS334 が足した・変えた C の全部（`git diff <p004 の基点> --stat -- '*.c' '*.h'` の一覧。module・統合・apps・試験の道具）を plan/coding-style.md の全文と §14 の checklist で読み、書き方を直す。
- 触れた block の開発の管理情報の comment（`ws074-pNNN` など、今の file にある物）を guardrail の規則で直す（design §9.1 の最後）。
- 実装の間に記録された header の comment の誤り（各 phase.md の結果の節と Q1 の受け入れの記録）を header に反映する（この Phase だけが header を変えてよい。直した後に `check-skeleton.sh` を BASE 無しで、`run-unit.sh all`・`cross-build.sh`・`wine-run.sh` を流す）。
- 結果: 直した file の数、checklist の項ごとの数、header の直しの一覧。commit、ws.md の行。

## 持ち越し（Q1）

- 2026-11-01 p009: `base/os-zones.h` の `struct wb_os_zone_names` の comment「名前は ASCII」を直す（CLDR 48 の Türkiye が UTF-8）。
- 2026-11-01 p013: `plan/ws334/tests/run-unit.sh` が win32 の試験で locale を `C.UTF-8` にする（host の ja_JP.UTF-8 が無いと Wine が英字でない名前の file を作れない）。
- 2026-11-01 p008: `base/os-time.h` の Windows の時差の comment の `SystemTimeToTzSpecificLocalTimeEx` を、実装の `GetTimeZoneInformationForYear`＋`SystemTimeToTzSpecificLocalTime` に合わせる（前者は Wine で stub）。DST の試験を harness に入れる方法（Wine の TZ）も検討する。

## 対象の file（2026-11-15 P7 q2100 が決めた。確定は Q1）

基点からの diff は他の WS を含み 1651 file で使えないので、WS334 が作った file の全文と、その試験の C を対象にした（103 file、約 6 万行、`style-check.py` で数えた）。

1. libbrowser の OS の層と保存: `libbrowser/base/os*.{c,h}`（22）、`storage/{records,files,queue,instance}.{c,h}`。
2. `libmedia/media-os*.{c,h}`、`libkeiland/win32/*.{c,h}`（22）、`libkeiland/platform/{platform,socket,thread}-win32.c`、`apps/browser-win32/*.{c,h}`・`tools/*.{c,h}`、`include/browser/storage.h`。
3. 試験: `plan/ws334/tests/*.c`・`unit/*.{c,h}`・`win32-posix.h`、`userland/tests/browser-threads/threads-posix.{c,h}`。
4. 共有の file の WS334 の block（`net/loader.c`・`cache.c`、`page.c`、libmedia の wake、shell の `threads.c`・`main.c`、libtruetype `companion.c`）は今回は読んでいない（未実施、下）。

`apps/browser-win32/Makefile` の source の一覧との照合は未実施。

## 結果（2026-11-15 P7）

直した物（動作は変えない）:

- header の持ち越し 3 件: `os-zones.h`（名前は UTF-8、ASCII でなく Türkiye を含む）、`os-time.h`（`GetTimeZoneInformationForYear`＋`SystemTimeToTzSpecificLocalTime` に合わせた）、`run-unit.sh`（Wine を `LC_ALL=C.UTF-8` で）。
- 試験の道具の壊れ: `modules.sh` の `os-time:posix` に `ws334_threads` を足した（p040 の後、基準の `run-unit.sh all` が os-time の posix の link で FAIL していた）。
- design.md: winpthreads・OpenSSL・vulkan-1.dll の遅延の import・`browser-win64`/`browser-winarm64` の top の target を今の形（OS の層の Win32 thread、libssl-compat・libcrypto-compat、普通の import、`make browser-win32`）に直した（先頭に「今の形」の節、§0・§3.4・O3/O5/O10/O17・M5/M6・§6.1/6.2/6.6/6.8/6.9/6.10・§11 A17・§12 R3・§9 ほか）。
- `openssl-windows.sh`・`openssl-conf/` の参照: phase030（注）・BUG-435 の inventory（行を削除）・ws335 phase036（注）を直した。ws.md・design.md は直した。file の `git rm` は Q1 の OK の後。
- C の書き方（機械の検査 `plan/tools/style-check.py` の基準は 1504 件）: 条件の中の関数の呼び出しを変数に（os-file-win32・os-file-posix・os-win32・os-net-win32・files・queue・pointer・ime・drag・clipboard・wide・titlebar・window・present の `IsEqualIID`・`os_is_*`・`os_has_drive`・`strcmp`・`wcscmp`・`wcsncmp`・`TrackMouseEvent`・`GetMonitorInfoW`・`WSAGetLastError`・`OpenClipboard`・`titlebar_side`・`titlebar_state`・`drag_kind`・`is_*_surrogate`・tsf-main の `IsEqualIID`）、`S_ISDIR`・`S_ISREG` を `S_IFMT` の比較に、条件演算子（入れ子・複数行・引数の中）を `if` に、閉じ括弧の後の空行とその段落の comment、判断の無い段落の comment。短い `text == NULL ? "" : text` などの対称な選びは残した。
- comment の中の開発の管理情報: test の 4 file の `WS334's`・`plan/` の path を直した（`#include` の path は code なので残した）。

機械の検査の今の数（style-check、対象の 103 file）は下の表。残りは「再開点」。

## 再開点（残り、2026-11-15 P7）

- 試験の C（`plan/ws334/tests/unit/test-*.c`、`storage-test.c` ほか）: 段落の comment（約 470）、閉じ括弧の後の空行（約 230）、静的な関数の前方宣言（約 110、test-os-file・test-os-net・test-os-time・test-os-win32・test-os-system・test-queue）、関数の先頭の宣言でない宣言（約 75、test-wide が 42）、条件の中の関数（`strcmp`・`strlen`・`test_pattern` など約 100）。直し方は production と同じ。
- `FAILED(...)`・`SUCCEEDED(...)`・`HIWORD(...)` は Windows の macro（値の比較）であり関数の呼び出しではないが、`style-check.py` は数える（production 約 35 件）。`(void)param;` の列と `is_local`（`files.c`）は検査の偽陽性。`goto cleanup`・`goto done`（`os-net-win32.c` 10、`socket-win32.c` 13）は共有の cleanup への前方への 1 回の jump で規約に合う。
- 共有の file の WS334 の block（上の 4）と、`return f(...)` の直接の return・引数の中の入れ子の呼び出し・comment の意味の確かめ（機械で見えない規則）は、全 file を目で読む所までは進んでいない。
