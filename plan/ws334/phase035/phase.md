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

## 対象の file の一覧の決め直し（案、2026-11-14 q2095-i01 P7。決めるのは Q1。この Phase の作業はまだ始めていない）

- 上の「`git diff <p004 の基点> --stat`」は今は使えない。基点 d393d3267 からの差は他の WS の変更を含み、`*.c`・`*.h` だけで 1651 file（2026-11-14 に数えた）。WS334 の変更に絞る形に替える。
- 案: 次の 3 つを対象にする。
  1. WS334 が作った file の全文: `userland/desktop/libbrowser/base/os*.{c,h}`（22）、`storage/records.{c,h}`・`files.{c,h}`・`queue.{c,h}`・`instance.{c,h}`、`userland/desktop/libmedia/media-os*.{c,h}`（3）、`userland/desktop/libkeiland/win32/*.{c,h}`（22）、`userland/desktop/libkeiland/platform/platform-win32.c`・`socket-win32.c`・`thread-win32.c`、`apps/browser-win32/*.c`・`tools/*.{c,h}`、`userland/desktop/include/browser/storage.h`。
  2. WS334 の試験の C（`plan/ws334/tests/*.c`・`unit/*.{c,h}`、37）と `userland/tests/browser-threads/threads-posix.{c,h}`。
  3. 共有の file のうち WS334 の Phase が変えた block だけ（各 phase.md の commit の表から拾う。例: libbrowser の `net/loader.c`・`net/cache.c` の cache の口、`page.c` の drive の path、`libmedia` の wake の callback、shell の `threads.c`・`main.c` の Windows の分岐、libtruetype `companion.c`）。block の外は直さない。
- 書き方の直しの前に、`apps/browser-win32/Makefile` に一覧の無い file（`git ls-files` と Makefile の source の一覧の照合）が無いことを確かめる。
- design.md の古い記述（§3.4・O3・O5・O10 ほかの winpthreads、OpenSSL の当面の利用、vulkan-1.dll の遅延の import、`browser-win64`・`browser-winarm64` の top の target）は、この Phase で今の形（p040・ユーザーの決定、libssl-compat、普通の import、`make browser-win32`）に直すか、別に直すかを Q1 が決める。
- p037 の `ime.c`・`pointer.c` の COM の呼び出しには、`IsEqualIID` を条件の中で呼ぶ所など coding-style §6 の「条件の中で関数を呼ばない」に合わない書き方が drag.c と同じ形で残る（p037 は動作を優先した）。この Phase の対象に入れる。
