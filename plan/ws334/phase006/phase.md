<!-- awesome-plan project=zedbsd record=ws334-p006 -->

# ws334-p006 file: os-file

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-11-01 04:07 Q1、main 782c4483d）
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
- path_from_url・url_from_path: header の各場合を表で（posix: host 空・"/a b/c" はそのまま、host "srv" → ENOENT、"rel" → EINVAL。win32: "/C:/x/y" → "C:\\x\\y"、"/C:" → "C:\\"、"/c|/a" → "c:\\a"、"/x" → ENOENT、host "srv" と "/share/a" → ENOENT（他の機械の file: URL は開かない。header の理由）、"C:\\a\\b" ↔ "/C:/a/b"、"C:/a" → "/C:/a"、"\\\\srv\\share\\x" → host "srv"・path "/share/x"、"C:rel"・"\\\\?\\C:\\x"・"\\\\.\\COM1"・"rel" → EINVAL）。失敗の時に buffer の長さが変わらない。
- path_is_absolute: posix "/"・"/a" → 1、"a"・""・NULL → 0。win32 "C:\\x"・"c:/x"・"\\\\srv\\x"・"//srv/x" → 1、"C:"・"C:x"・"\\x"・"/x"・"x" → 0（試験は両方の system で両方の表を流さず、自分の system の表だけ）。
- mode（posix）: replace の file が 0600、make_directories の directory が 0700、lock の file が 0600（umask 022 の下で `stat` で）。write は 0644 と umask。
- Windows の共有: `wb_os_file_read` の handle を開いたまま（試験が別の handle で読みを開いておく形ではなく、読んでいる途中に別の thread が `wb_os_file_replace` を呼ぶ形は作りにくいので）、試験が `CreateFileW(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)` で開いた handle を持ったまま `wb_os_file_replace` が成功する（読み手が置き換えを止めない約束の確かめ）。
- make_directories の path 自身が file → ENOTDIR、途中が file → ENOTDIR（Windows は `GetFileAttributesW` で見分ける）。
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
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

日付: 2026-11-01（host の `date`）。担当 Sonnet 5.5。実行は `LC_ALL=C.UTF-8`。

作った物: `os-file-posix.c`・`os-file-win32.c`（骨格を実装）、`plan/ws334/tests/unit/test-os-file.c`。header・他の module・Makefile・試験の道具は変えていない。

実行した command と最後の行:

- `sh plan/ws334/tests/run-unit.sh all`: `test-os-file: PASS (1642 checks) (posix)`、`test-os-file: PASS (1710 checks) (win32)`、他の試験（frame・keys-table・media-os・os-system・os-win32・os-zones・records・tools）も PASS、`run-unit: PASS`。
- `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh`: `headers: 19`、`sources: 22`、`check-skeleton: PASS`。
- `git diff --check`: 出力なし。

わざと壊した確認（tracked の file は変えず、`build/tmp/mut/` の複製の module を試験に繋いで走らせた。元の file は一度も変えていない）: 23 通り（ほかに compile が通らない壊し方 2 つは除く）。FAIL で検出 18: UTF-8 の 0xF4 の上限、directory の mode（0700→0755）、replace の file の mode（0600→0644）、write の mode（0644→0666、umask 0 で見る）、posix の `path_from_url` の host の拒否、置き換えが rename に失敗した時の一時 file の削除、`wb_os_list` の stop の伝播（posix・win32）、`make_directories` の mid-path が file の ENOTDIR（win32）、`remove` の directory の EISDIR（win32）、win32 の `path_from_url` の drive の後の切り出しの 1 つのずれ、`url_from_path` の server の長さの 1 つのずれと UNC の `\\?\` の拒否、`".."` を出さない検査（win32）、MoveFileExW のやり直しの回数（5→0）、`LockFileEx` の EXCLUSIVE の抜き、`path_is_absolute` の「2 つの separator の後が separator」の検査、lock の `O_CLOEXEC` の抜き（子の process が lock を持ち続ける形で検出するため、試験を足した）。見逃し 5 つはいずれも試験で見分けられない形の差:
  - 同じ process の lock の一覧の照合を外す（posix）: Linux の flock は open file description ごとなので、一覧が無くても同じ process の 2 回目は EBUSY になる。header が書く「flock が通ってしまう system」でしか差が出ない。
  - `wb_os_file_read` の読みが途中で失敗した時の buffer の長さの復元（posix）: 途中で EIO になる file を作れない。
  - 末尾の separator の切り落とし（posix `make_directories`）: 名前の無い部分を飛ばすので結果が同じ（等価）。
  - win32 の `os_root_length` の `\\?\C:\` を 6 にする: 同じく名前の無い部分を飛ばす（等価）。
  - win32 の `GetCurrentDirectoryW` の buffer の +1: 返る長さが NUL を含むので +1 は余りで、等価。

自分で決めた点（header が黙っている所）:

- POSIX の path も UTF-8 を検査し、誤りは EILSEQ（os.h の「text は UTF-8」と、一覧が UTF-8 でない名前を飛ばす約束に揃えた）。`wb_os_full_path`・`wb_os_current_directory` の答えが UTF-8 でなければ EILSEQ。WB_OS_PATH_MAX を超える path は ENAMETOOLONG。
- errno の写し: POSIX は ELOOP→ENOENT、EPERM・EROFS→EACCES、ETXTBSY→EBUSY、EDQUOT→ENOSPC、ENFILE→EMFILE、その他の未知の値と EINVAL→EIO。read・write・replace・remove・full_path・lock の ENOTDIR は ENOENT にする（それらの戻り値の一覧に ENOTDIR が無く、Windows の ERROR_PATH_NOT_FOUND とも揃う）。`wb_os_list` と `make_directories` は ENOTDIR のまま。POSIX の unlink が directory に EPERM・EACCES を返す system では `lstat` で見分けて EISDIR。
- `make_directories` は戻り値の一覧に ENOENT が無いので、途中で消えた部分・無い drive の ENOENT は EIO にする。dangling symlink が途中にある時は ENOTDIR。root（"/"、"X:\"）は作らず、あるかだけを見る。
- 一時 file の名前は path + ".tmp" + counter + "." + process ID（header の語の順）。POSIX は counter を `__atomic_add_fetch`、Windows は `InterlockedIncrement`。名前の衝突（EEXIST）は次の番号で最大 100 回、尽きれば EIO。POSIX の replace は rename の後の directory の fsync をしない（header は「file を flush してから rename」）。
- POSIX の `wb_os_lock_open` は directory を `realpath` で解決した名前を一覧の鍵にする（"lk"・"lk/"・"./lk/../lk" が同じ）。lock の file は鍵 + "/lock"。
- `wb_os_path_from_url` の posix は header と net/url.c の通り空の host なら path をそのまま（"rel" も成功）。phase.md の「"rel" → EINVAL」は `wb_os_url_from_path`（"/" で始まらない → EINVAL）の事と読んだ。
- Windows: 読みの directory は CreateFileW の ACCESS_DENIED と `GetFileInformationByHandle` の属性で EISDIR（Wine は開けてしまうため両方で見る）。write の共有は FILE_SHARE_READ、一時 file は FILE_SHARE_READ、lock の file は header の通り READ|WRITE。MoveFileExW が ACCESS_DENIED で、destination が directory なら EISDIR（やり直さない）。`list` は FindFirstFileW の最初が FILE_NOT_FOUND でも directory なら空として成功（root の空の drive）、file なら ENOTDIR、無ければ ENOENT。
- Windows の `url_from_path`: "\\\\srv"・"\\\\srv\\" は host "srv"・path "/"。"//srv/x" の `/` 版の UNC も受ける。server が空（"\\\\\\x"）・"?" で始まる・"." だけの server 名の後に separator か末尾（device path）は EINVAL。`path_is_absolute` は "\\\\?\\x"・"\\\\.\\x" を絶対とし、"\\\\"・"\\\\\\x"・"//" は絶対でない。`path_from_url` は "/C:" の後が "/" か末尾でなければ（"/C:x"）ENOENT、"/C:/" は "C:\\"。
- 読みの buffer は、0 byte の file でも `wb_buffer_reserve(…, 0)` で確保して NUL を保つ。失敗は長さを戻して NUL を打ち直す。

Wine の制限（実機の Windows では未確認）: Wine 10.0 は、別の handle が FILE_SHARE_DELETE で開いている file への `MoveFileExW(…, REPLACE_EXISTING)` を ACCESS_DENIED で拒む（`ReplaceFileW` も不可。小さい exe で確認）。そのため「読み手が開いたまま replace が成功する」確かめ（共有の試験と thread の試験）は、Wine では EACCES を許し（`wine_get_version` で Wine を見分ける）、Wine でない Windows では成功を要求する。Wine でも確かめられた事: 共有の無い holder に対し 5 回・20 ms おきにやり直して EACCES か EBUSY を返す（90 ms 以上かかる）、30 ms 後に holder が閉じれば成功する、一時 file が残らない、読みと置き換えの thread の競争で一度も「混ざった内容」を読まない。

試験で使った時間の注意: この host の fsync は 2〜6 秒かかる（load average 13）。replace を多く呼ぶ試験は遅いので、replace の回数を絞ってある（`run-unit.sh os-file` の posix・win32 でそれぞれ数分）。

header の不備の指摘: なし（止める必要は無かった）。

未実施: 実機の Windows（上記の Wine の制限の部分と LockFileEx・MoveFileExW の実機の挙動）、aarch64 Windows の実行（compile だけ）、BSD・zedBSD での posix の実行（host の Linux のみ）、読みの途中の EIO、`libbrowser` の既存 code を新しい OS の層へ替える作業（p021）。
