<!-- awesome-plan project=zedbsd record=ws334-p011 -->

# ws334-p011 files の backend: files

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: in-progress（2026-11-01 04:08 q1807、Sonnet）。
- 依存: p006・p010（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.5.2〜§3.5.5、§3.7。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/include/browser/storage.h`、`userland/desktop/libbrowser/storage/files.h`、`userland/desktop/libbrowser/storage/records.h`、`userland/desktop/libbrowser/base/os-file.h`。それが include する header も読む。
4. 仕様: header だけ（`<browser/storage.h>` の backend の約束と `storage/files.h` の layout）。

## 作る物

- `userland/desktop/libbrowser/storage/files.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-files.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 開く: 無い directory を親ごと作る、同じ directory の 2 回目の open → EBUSY（`*calls`・`*context` NULL）、close の後は開ける、NULL・空 → EINVAL、file を directory に渡す → ENOTDIR。table の version が BROWSER_STORAGE_VERSION で close 以外が全部 NULL でない。
- 3 つの space と他の space（7）の write・read・remove・list・clear の往復。read は無い record で found 0 と sink に何も足さない、space の directory が無い時も found 0。write の後の read は同じ bytes（0 byte・1 MiB を超えて sink の append が複数回になる 3 MiB）。上書き。remove の無い key は 0。
- list: 書いた key の集まりと一致、一時の file（"<key>.tmp…"）・不正な名前（"ABC"・"x y"）・local-storage の ".ls" の無い file を出さない。each が 4 を返すと 4。clear は list の record を消し、不正な名前の file を残す。
- layout: cookie の record が `<directory>/cookies/<key>`、LOCAL が `<directory>/local-storage/<key>.ls`、space 7 が `<directory>/space-7/<key>`、`<directory>/lock` がある（試験が `wb_os_file_read` で直に見る）。
- 今の localStorage の file: 試験が `<directory>/local-storage/<origin の 16 進>.ls` に version 1 の bytes を置き、LOCAL の key（`wb_storage_key_text` の origin の物）で read すると同じ bytes。
- 不正な key（""・"A"・201 文字）の read・write・remove → EINVAL。
- 別の process: 試験が自分を "files-child <directory>" で起動し、子の `browser_storage_files_open` が EBUSY。

## 注意

- OS の呼び出しは `base/os-file.h` だけ（`#ifdef` を書かない）。path は `wb_buffer` で組み立てる（区切りは '/'。Windows も '/' を受ける）。
- read の sink への append は WB_STORAGE_FILES_PIECE ごと。sink の誤りはそのまま返す。
- 自分の lock の持ち方は `wb_os_lock_open`（同じ process の 2 回目は os-file が EBUSY にする）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh files` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
