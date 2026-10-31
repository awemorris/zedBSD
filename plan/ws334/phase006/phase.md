<!-- awesome-plan project=zedbsd record=ws334-p006 -->

# ws334-p006 file: os-file

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p005（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.4 の file・path と URL の行、§3.5.5（置き換え・OS の lock）、§2.2 の O13〜O16・O18。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libbrowser/base/os.h`、`userland/desktop/libbrowser/base/os-win32.h`、`userland/desktop/libbrowser/base/os-file.h`、`userland/desktop/libbrowser/base/base.h（wb_buffer）`。それが include する header も読む。
4. 仕様: POSIX.1-2024 の `open`・`read`・`write`・`fsync`・`rename`・`unlink`・`mkdir`・`opendir`・`readdir`・`realpath`・`getcwd`・`flock`（BSD・Linux）。Microsoft Learn の `CreateFileW`・`ReadFile`・`WriteFile`・`FlushFileBuffers`・`MoveFileExW`・`DeleteFileW`・`CreateDirectoryW`・`FindFirstFileW`・`FindNextFileW`・`GetFullPathNameW`・`GetCurrentDirectoryW`・`GetFileAttributesW`・`LockFileEx`。手元に置く文書は無い。

## 作る物

- `userland/desktop/libbrowser/base/os-file-posix.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libbrowser/base/os-file-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-os-file.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 読み書き: 0・1・65536・3 MiB の byte の往復（`\r\n`・`\0`・0x1a を含み、Windows で text の mode の変換が無いこと）。読みは buffer の既の中身の後に足し、後ろに NUL。無い file → ENOENT で buffer の長さが変わらない。directory を読む → EISDIR。無い directory の中への書き → ENOENT。
- 置き換え: 新しい file、既にある file の上書き、大きい file から小さい file へ。終わった後に一時の file（名前が path + ".tmp" で始まる物）が残らない（`wb_os_list` で見る）。directory が無い時 ENOENT で一時の file も無い。
- remove: 成功、無い → ENOENT、directory → EISDIR。
- make_directories: "a/b/c" を作る、2 回目も 0、末尾の区切り、途中が file → ENOTDIR。
- list: 作った名前の集まりと一致（順は問わない）、"."・".." が出ない、each が 5 を返すと 5 で止まる、無い directory → ENOENT、file → ENOTDIR。
- UTF-8 の名前: "データ.txt"・"emoji-😀.bin" を書き・list・読み・remove。誤りの UTF-8 の名前（"\xff.txt"）→ EILSEQ。
- full_path: posix は既にある file の結果が `wb_os_current_directory` + "/" + 名前、symbolic link を辿る、無い file → ENOENT。win32 は "a\\..\\b" が current + "\\b"（存在を見ない）。
- current_directory: posix は `getcwd` と、win32 は `GetCurrentDirectoryW` の UTF-8 と一致（試験が別の道で求める）。
- path_from_url・url_from_path: header の各場合を表で（posix: host 空・"/a b/c" はそのまま、host "srv" → ENOENT、"rel" → EINVAL。win32: "/C:/x/y" → "C:\\x\\y"、"/C:" → "C:\\"、"/c|/a" → "c:\\a"、"/x" → ENOENT、host "srv" と "/share/a" → "\\\\srv\\share\\a"、host と "/" → ENOENT、"C:\\a\\b" ↔ "/C:/a/b"、"C:/a" → "/C:/a"、"\\\\srv\\share\\x" → host "srv"・path "/share/x"、"C:rel"・"\\\\?\\C:\\x"・"\\\\.\\COM1"・"rel" → EINVAL）。失敗の時に buffer の長さが変わらない。
- lock: 開く・同じ process で 2 回目 → EBUSY・閉じて開き直せる・lock の file が残る・無い directory → ENOENT。別の process: 試験が自分を引数 "lock-child <directory>" で起動し（posix は `posix_spawn`、win32 は `CreateProcessW`）、子が `wb_os_lock_open` の値を exit の code で返す → EBUSY。親が閉じた後の子 → 0。
- 共通: NULL・空の path → EINVAL。ASan・UBSan の報告 0。

## 注意

- 置き換えの一時の file の番号は process の中の counter（atomic か lock）と process の ID。Windows の `MoveFileExW` のやり直しは header の通り（EACCES・EBUSY で 20 ms おきに最大 5 回）。
- POSIX の lock は同じ process の中の 2 回目を EBUSY にするため、開いている directory の一覧（`wb_os_full_path` の形で比べる）を lock 付きで持つ。
- Windows の list は `FindFirstFileW` の "\\*"。`wb_os_full_path` の Windows の側は `GetFullPathNameW` の長さを求めて確保する。
- file: の URL の変換の規則は header の通り（今の `net/url.c:412-459` の posix の挙動を変えない）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-file` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
