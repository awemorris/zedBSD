<!-- awesome-plan project=zedbsd record=ws334-p013 -->

# ws334-p013 libmedia の OS の層: media-os

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §5 の M2・M4・M9。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libmedia/media-os.h`。それが include する header も読む。
4. 仕様: POSIX.1-2024 の `dlopen`・`dlsym`・`open`・`fstat`・`pread`・`close`。Microsoft Learn の `LoadLibraryExW`（`LOAD_LIBRARY_SEARCH_SYSTEM32`）・`GetProcAddress`・`CreateFileW`・`GetFileSizeEx`・`ReadFile`（`OVERLAPPED` の `Offset`・`OffsetHigh`）・`GetFileAttributesW`・`MultiByteToWideChar`。手元に置く文書は無い。

## 作る物

- `userland/desktop/libmedia/media-os-posix.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libmedia/media-os-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-media-os.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- vulkan: posix は host の `libvulkan.so.1`（host にある）が開き `vkGetInstanceProcAddr` が NULL でない。win32 は Wine の system32 の `vulkan-1.dll` が開き同じ。無い symbol → NULL、NULL の引数 → EINVAL・NULL。
- file: 0 byte・1 byte・5 MiB の file を試験が作り、size が正しい、先頭・中・末尾の read_at、size 0 の read は 0、末尾を越える read → EIO、offset + size の 64 bit の overflow → EINVAL、無い file → ENOENT、directory → EISDIR、UTF-8 の名前（"動画.mp4"）、誤りの UTF-8 → EILSEQ、close(NULL)。
- 読んでいる間に他の handle で書き足しても開ける（win32 の共有の flag）。
- `media_os_close_descriptor`: posix は `pipe` の端を閉じて `fcntl(F_GETFD)` が EBADF、負の値は何もしない。win32 は何もしない。

## 注意

- libmedia は libbrowser の base を使わない。 Windows の誤り: directory を開くと ERROR_ACCESS_DENIED が来るので、`GetFileAttributesW` で directory なら EISDIR にする。path の途中が file の時の ERROR_PATH_NOT_FOUND は ENOENT。UTF-8 → UTF-16 は `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` を直に。誤りの写しは media-os-win32.c の中に（ENOENT・EACCES・EISDIR・ENOMEM・EIO の分だけ）。
- Windows の read_at は同期の handle に `OVERLAPPED` の offset で `ReadFile`、1 GiB ずつ、短い読みは続ける、0 byte は EIO（縮んだ）。
- `(void)` で使わない引数を消す（libmedia に UNUSED_PARAMETER は無い）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh media-os` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
