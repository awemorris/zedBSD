<!-- awesome-plan project=zedbsd record=ws334-p016 -->

# ws334-p016 libkeiland の platform の Windows: platform-win32

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §2.7（platform の層）、§4.5 の内部の行、WS327 の design §5（thread の Windows の考え方）。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libkeiland/platform/platform.h`。それが include する header も読む。
4. 仕様: Microsoft Learn の `CreateEventW`（manual-reset、名前なし、継承しない）・`SetEvent`・`ResetEvent`・`CloseHandle`・`SetThreadDescription`（実行時に `GetProcAddress` で）。thread と lock は `<pthread.h>`（llvm-mingw の winpthreads）。手元の file は無い。今の `platform-posix.c` の挙動（同じ約束の POSIX の実装）を読んで同じ意味にする（code は写さず、約束を合わせる）。

## 作る物

- `userland/desktop/libkeiland/platform/platform-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-platform-win32.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 試験は win32 だけ（modules.sh で posix は none）。
- wake: open で event が NULL でなく ends が -1・-1、`keiui_wake_descriptor` が -1、ring の後に `WaitForSingleObject(keiui_wake_event(w), 0)` が WAIT_OBJECT_0、clear の後は WAIT_TIMEOUT、ring を 2 回して clear 1 回で鳴っていない、別の thread の ring が `WaitForSingleObject(..., INFINITE)` で待つ thread を起こす（100 回）、close の後に event の handle が閉じている（`GetHandleInformation` が失敗）。
- lock: take・give、2 つの thread で 100000 回ずつ counter を増やして合計が正しい。
- thread: `keiui_thread_self` と `keiui_thread_is`（自分は 1、他の thread の id は 0）、`keiui_thread_start`（join しない thread が走って印を立てる）、`keiui_thread_start_joined` と `keiui_thread_join`、`keiui_thread_name`（NULL・長い名前で落ちない）。

## 注意

- POSIX の file と同じ約束を Windows で: wake は manual-reset の event（ring は SetEvent、clear は ResetEvent、何度鳴らしても 1 つ）。`keiui_wake_event` は event を返す。open の失敗は今の約束の誤りの値（ENOMEM か EIO。`GetLastError` の写しは file の中の static の関数で）。
- process（`keiui_child` と process.h の物）は Windows では作らない（この Phase の外。p028 の stub）。
- `<windows.h>` を include する。`platform.h` は変えない。
- POSIX の file（`*-posix.c`）と OS に依らない file に `#ifdef _WIN32`・`<windows.h>` を書かない（`check-skeleton.sh` が見る）。Windows の file（`*-win32.c`）は `<windows.h>`・`<winsock2.h>` などを include してよいが、W の API だけを使い（A の API を使わない）、`<jpeglib.h>` を include しない。
- 他の実装（Chromium・Firefox・Wine・curl・libuv・glib・Qt など）の code・comment・構造・名前を写さない。Win32・POSIX の API の使い方は API の reference から書く。
- source の comment に WS・Phase の番号・plan/ への参照・日付を書かない。各関数は coding-style の形（multi-line の定義、宣言の群、comment の付いた段落、成功の return を最後に）。
- 試験は header の約束を全部の誤りの場合まで確かめる。期待の値は header・API の reference・手の計算から作り、他の実装の出力を使わない。Windows の側の期待を Windows の API で独立に求めてよい（試験の file の中で、実装と別の道で）。
- 試験の file は `plan/ws334/tests/unit/test-<名前>.c`。`#include "check.h"` の `CHECK`・`CHECK_EQ`・`check_finish("<名前>")` を使い、`main(argc, argv)` の `argv[1]` が "posix" か "win32"。今の directory は試験ごとの空の directory で、file を作ってよい。

## 変えてはいけない物

- 全部の module の header（`plan/ws334/tests/modules.sh` の `ws334_headers` の全部）。header の不備・矛盾・足りない口を見つけたら直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。header が黙っている所は自分で決め、「結果」に列挙する。
- 担当の外の `.c`（他の Phase の module、libbrowser・libmedia の既存の file）、`apps/browser-win32/Makefile`・一番上の `Makefile`、試験の道具（`plan/ws334/tests/modules.sh`・`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/test-tools.c`）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。push しない。

## 確かめの command（worktree の root で）

- `timeout 1800 sh plan/ws334/tests/run-unit.sh platform-win32` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
