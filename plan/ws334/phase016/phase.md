<!-- awesome-plan project=zedbsd record=ws334-p016 -->

# ws334-p016 libkeiland の platform の Windows: platform-win32

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-11-01 03:30 Q1、main 174e9ed0c）
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
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

- 状態: 実装済み・試験 PASS（2026-11-01 host の `date` で 03:30 ごろ。cleared は Q1 の判定）。
- 作った物: `userland/desktop/libkeiland/platform/platform-win32.c`（全部の関数）、`plan/ws334/tests/unit/test-platform-win32.c`。header・試験の道具・他の module は変えていない。
- 試験: `LC_ALL=C.UTF-8 sh plan/ws334/tests/run-unit.sh platform-win32` → `test-platform-win32: PASS (313 checks) (win32)`、`run-unit: PASS`（posix は modules.sh で none。Wine の prefix 作りを含めて 5 分）。
  `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`、`git diff --check` は出力なし。`LC_ALL=C.UTF-8 sh plan/ws334/tests/run-unit.sh all` → 全部 PASS（platform-win32 は 313 checks、他の test も posix・win32 とも PASS）、最後の行 `run-unit: PASS`。
  試験は、wake の open（garbage の構造への書き込み・event が非 NULL・ends が -1・descriptor が -1・新しい event が quiet・非継承）、ring の後の signaled が wait で消えない（manual-reset）、
  clear で quiet、2 回 ring して 1 回 clear で quiet、quiet への clear、システムの SetEvent・ResetEvent との往復、2 つの wake の独立、別の thread の ring が wait を起こす（100 回。ring が wait の前・後の両方）、
  1 回の ring が 2 つの待ち手を起こす、close の後に `GetHandleInformation` が失敗・構造が NULL と -1・2 回目の close と閉じた wake への ring・clear が無害・同じ構造への再 open、
  lock の open・close の繰り返しと 2 つの thread の 100000 回ずつの加算（合計 200000）、保持中の lock が別の thread を止めて give で進む、
  thread の self と is（自分は 1、別の thread は 0、start_joined が返す id が thread 自身の self と同じ）、join しない thread が引数を受けて印を立てる、
  run または thread の NULL の拒否（EINVAL）、`keiui_thread_name`（NULL・短い名前・1・2・3 byte の字・63 byte ちょうど・200 byte・字の途中の切断（3 byte と 2 byte）・UTF-8 でない名前・空の名前）を `GetThreadDescription` で読み戻して見る。
- 壊して確かめた（git の外で 1 つずつ。各回 Wine で試験を流し、source は無傷）: event を auto-reset にする・初期を signaled にする・descriptor が 0 を返す・ring が ResetEvent・clear が SetEvent・close が CloseHandle しない・
  close が event を NULL にしない・open が ends を -1 にしない・event を継承可能にする・keiui_wake_event が NULL を返す・lock_take が何もしない・lock_give が何もしない・thread_is の反転・
  join が何もしない・start_joined の NULL の検査の欠落・start の NULL の検査の欠落・引数を渡さない・名前の上限を 62 にする・字の途中の切断の戻しを外す（2 通り）・MB_ERR_INVALID_CHARS を外す・
  名前の NULL の検査を外す・UTF-16 の終端を壊す、を全部 FAIL（または hang の timeout、または trap）で捕まえた。
  捕まえられない物: `platform_wake_error` の ENOMEM・EIO の写し（CreateEventW の失敗を起こせない）、`pthread_create` の失敗の返し、detached の属性（外から見えない）。
- 自分で決めた点（header が黙っている所）:
  1. 閉じた（または開いていない NULL の）wake への ring・clear は何もしない。close は 2 回呼んでも無害で、event を NULL、ends を -1 に戻す。open は失敗しても ends を -1、event を NULL にする。
  2. lock は header の `pthread_mutex_t`（winpthreads）をそのまま使う（design の SRWLOCK・CRITICAL_SECTION の語は header の型に合わせて採らない）。thread も winpthreads の `pthread_create` で、signal は無いので blocking しない。
  3. `keiui_thread_start`・`keiui_thread_start_joined` は run が NULL なら EINVAL、joined は thread が NULL でも EINVAL（POSIX の file は検査しない。hostile な入力で落ちないため）。
  4. `keiui_thread_name`: NULL は何もしない。`SetThreadDescription` は kernel32.dll から `GetProcAddress` で実行時に探し、無ければ何もしない。UTF-8 は 63 byte で切り、字の途中なら字の前まで戻す。
     UTF-8 として正しくない名前は MB_ERR_INVALID_CHARS で拒否し、名前を変えない。空の名前は空の名前にする。
  5. open の失敗の値は ERROR_NOT_ENOUGH_MEMORY・ERROR_OUTOFMEMORY・ERROR_NO_SYSTEM_RESOURCES・ERROR_COMMITMENT_LIMIT が ENOMEM、他は EIO。
- 試験の道具について: 他の agent と scratchpad が共有で、同名の file（mut.sh）を上書きされた。壊す試験は自分の directory で流し直した。
- 未実施: 実際の Windows での実行（Wine 10.0 のみ）、aarch64 Windows の実行（compile のみ）。
