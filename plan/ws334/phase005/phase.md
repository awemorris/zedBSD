<!-- awesome-plan project=zedbsd record=ws334-p005 -->

# ws334-p005 Win32 の補助と system の services: os-win32・os-system

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-11-01 03:11、cleared は Q1）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.4 の表（memory・乱数・thread・library・場所・根の証明書・名前の行）、§6.6（根の証明書の使い方）、§2.2 の O7・O21・O23・O30。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libbrowser/base/os.h`、`userland/desktop/libbrowser/base/os-win32.h`、`userland/desktop/libbrowser/base/os-system.h`。それが include する header も読む。
4. 仕様: POSIX.1-2024 の `posix_memalign`・`dlopen`・`dlsym`・`dlclose`、Linux・FreeBSD の `pthread_setname_np`。Microsoft Learn の `_aligned_malloc`・`_aligned_free`・`BCryptGenRandom`・`SetThreadDescription`・`GetProcAddress`・`LoadLibraryExW`・`GetModuleFileNameW`・`CertOpenStore`（`CERT_STORE_PROV_SYSTEM_W`、`CERT_SYSTEM_STORE_CURRENT_USER`・`_LOCAL_MACHINE`、`CERT_STORE_READONLY_FLAG`）・`CertEnumCertificatesInStore`・`MultiByteToWideChar`・`WideCharToMultiByte`（`MB_ERR_INVALID_CHARS`・`WC_ERR_INVALID_CHARS`）、`<winerror.h>`・`<winsock2.h>` の誤りの値（llvm-mingw の `x86_64-w64-mingw32/include/` で値を確かめる）。手元に置く文書は無い（約束は header に書いた）。

## 作る物

- `userland/desktop/libbrowser/base/os-win32.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libbrowser/base/os-system-posix.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libbrowser/base/os-system-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-os-win32.c`。
- 単体試験 `plan/ws334/tests/unit/test-os-system.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- `test-os-win32.c`（win32 だけ。modules.sh で posix は none）:
  - `wb_os_error_from_system`: header の表の全部の code が書いた値に写る、0 → 0、表に無い値（1・999・0xffffffff）→ EIO。
  - `wb_os_error_from_socket`: 表の全部、0 → 0、表に無い値（10000・-1）→ EIO。
  - `wb_os_wide_from_utf8`: ASCII、2・3・4 byte の UTF-8（U+00E9、U+3042、U+1F600 は surrogate の組 0xD83D 0xDE00）、空の文字列（長さ 0）、length の出力。誤り: overlong（C0 80・E0 80 80・F0 80 80 80）、surrogate（ED A0 80）、U+10FFFF を超える（F4 90 80 80）、途中で切れた列（E3 81）、単独の継続 byte（80）、F5〜FF → EILSEQ で `*wide` NULL・length 0。NULL の引数 → EINVAL。WB_OS_PATH_MAX byte ちょうどは成功、+1 は ENAMETOOLONG。
  - `wb_os_utf8_from_wide`: 上の往復、count の指定と `(size_t)-1`、組でない high・low surrogate・count の中の NUL → EILSEQ、NULL → EINVAL。
  - `wb_os_wide_path`: 相対の "a/b\\c" → L"a\\b\\c"（接頭辞なし）、"C:/x/../y" → L"\\\\?\\C:\\y"、"\\\\srv\\share\\x" → L"\\\\?\\UNC\\srv\\share\\x"、既に "\\\\?\\" で始まる物はそのまま、300 文字を超える drive の path が接頭辞付きで返り、それで `CreateFileW` が file を作れる（Wine は MAX_PATH の制限が無いので、作れることだけを確かめる）、空 → EINVAL、誤りの UTF-8 → EILSEQ。
- `test-os-system.c`（両方）:
  - `wb_os_aligned_alloc`: alignment sizeof(void *)・16・64・4096・65536 で番地が倍数、block の全部に書いて読める、`wb_os_aligned_free`。誤り: alignment 0・3・sizeof(void *)/2・131072、size 0、memory NULL → EINVAL（`*memory` NULL）。`wb_os_aligned_free(NULL)` は何もしない。
  - `wb_os_random`: length 0（bytes NULL も）は 0、bytes NULL と length 1 は EINVAL。1 MiB を 2 回取り、両方が全部 0 でなく互いに違う、256 の値が全部 1 回以上出る。
  - `wb_os_thread_name`: NULL・空・"browser-network"・40 文字の名前を主の thread と pthread_create した thread で呼んで落ちない。
  - `wb_os_library_open`: posix は {"libzedbsd-no-such.so", "libm.so.6", NULL} で 2 番目が開き `cos` を引いて `cos(0.0) == 1.0`、win32 は {"no-such-ws334.dll", "kernel32.dll", NULL} で `GetTickCount64` を引いて呼べる。無い symbol → NULL。全部無い → ENOENT（`*library` NULL）。names NULL・空の配列・空の名前 → EINVAL。`wb_os_library_close(NULL)`。
  - `wb_os_platform_name`・`wb_os_user_agent_system`: posix は "zedBSD x86_64"（host は x86_64。`include/kern/os-name.h` の ZEDBSD_OS_NAME と機械の名前から試験の中で組み立てて比べる）と "X11; Linux x86_64"、win32 は "Win32" と "Windows NT 10.0; Win64; x64"。
  - `wb_os_data_directory`: posix は "/usr/share"。win32 は試験が `GetModuleFileNameW` で求めた .exe の directory + "\\share"。小さい buffer（必要の長さちょうど・1 byte 足りない）→ ERANGE と空の文字列、size 0・NULL → EINVAL。
  - `wb_os_trust_roots`: posix は each が 1 回も呼ばれず 0。win32（Wine の ROOT の store）は 0 を返し、渡る DER が全部 0x30 で始まり長さ 1 以上、distrusted が 0 か 1。each が 77 を返すと 77 で止まる（ROOT が空なら止まりの確かめは記録して飛ばす）。each NULL → EINVAL。Wine に無い store（machine の Disallowed など）があっても 0 で、他の store の証明書は渡る（header の「無い store は飛ばす」）。

## 注意

- `wb_os_error_from_*` は system を呼ばない純粋な写し（値は header の表）。表の数は多いので switch で 1 行ずつ、block の前に「Values referenced: <winerror.h>・<winsock2.h> of mingw-w64」と書く。
- 乱数の POSIX の側は今の `js/builtin_typed.c:1377`・`bind/environment.c:1228` の `/dev/urandom` の読み（短い読み・EINTR のやり直しを足す）と同じ源。
- thread の名前の POSIX の側は header の通り `WB_OS_THREAD_NAMES` の build の macro で `pthread_setname_np` を呼ぶ（`#if defined(__linux__)` のような system の名前の判定を書かない。`plan/tools/keiland-os-boundary/check.sh` の L1 に当たるため）。`run-unit.sh` の posix は `-DWB_OS_THREAD_NAMES=1` で build する。
- `SetThreadDescription` は `GetModuleHandleW(L"kernel32.dll")` と `GetProcAddress` で実行時に探す（無ければ何もしない）。名前は UTF-16 にして渡す。
- library の Windows の側は `LoadLibraryExW` の flag を header の通りに（current directory・PATH を探さない）。
- data の directory の Windows の側は `GetModuleFileNameW` の buffer を足りなければ広げてやり直す（MAX_PATH に限らない）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-win32 os-system` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
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

日付: 2026-11-01（host の `date`）。担当 Sonnet 5.5。実行は `LC_ALL=C.UTF-8`（Q1 の注意: この host の LC_CTYPE=ja_JP.UTF-8 は未導入で Wine が非 ASCII の file 名を作れない。run-unit.sh は変えていない）。

作った物: `os-win32.c`・`os-system-posix.c`・`os-system-win32.c`（骨格を実装）、`plan/ws334/tests/unit/test-os-win32.c`・`test-os-system.c`。header・他の module・Makefile・試験の道具は変えていない。

実行した command と最後の行:

- `sh plan/ws334/tests/run-unit.sh all`: `test-os-system: PASS (220 checks) (posix)`、`test-os-system: PASS (231 checks) (win32)`、`test-os-win32: PASS (3126 checks) (win32)`、`test-tools` 両方 PASS、`run-unit: PASS`。
- `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh`: `check-skeleton: PASS`（headers 19、sources 22、`make browser-win64`・`browser-winarm64` 通る、header と道具は基点から不変）。
- `git diff --check`: 出力なし。
- 実装の compile は cc・clang（-DWB_OS_THREAD_NAMES の有無とも）、x86_64 と aarch64 の llvm-mingw で warning 0。

わざと壊した確認（git の外で変えて元に戻し、`cmp` で一致を確認）: 27 通り（誤りの写し 1 つ、UTF-8 の先頭 byte・第 2 byte の範囲、長さの境界 `>`/`>=`、surrogate の計算、UNC の接頭辞、alignment の上限、`ERANGE` の境界、乱数の読みの進み方と残りの数え、`BCryptGenRandom` の長さ、UA の文字、空の名前の検査、根の store の抜け、distrusted の値、stop の伝播、LoadLibraryExW の flag ほか）。26 は FAIL で検出。最初は見逃した 2 つ（乱数が複数回の read になる大きさ、現在の directory に置いた DLL を読まないこと）は試験を足して検出するようにした。見逃しが残るのは 1 つ: `Disallowed` の store の distrusted を 0 にする壊し方。Wine でその store は空で、試験から書き込んでも（CertAddEncodedCertificateToStore は成功するが）enumerate に現れず、distrusted 1 の経路を Wine では確かめられない。実機の Windows で確かめる必要がある（未実施）。

自分で決めた点（header が黙っている所）:

- `wb_os_wide_path`: 絶対と見なすのは「drive 文字 + `:` + separator」と「`\\` で始まる」だけ。`C:rel`・`\rooted` は相対として separator の変換だけ。`\\?\`・`\\.\` で始まる物は（`/` を `\` に変えた後）そのまま。GetFullPathNameW の結果が `\\?\`・`\\.\` で始まる時もそのまま。prefix 付きの長さが 32767 unit を超えると ENAMETOOLONG、GetFullPathNameW が ERROR_FILENAME_EXCED_RANGE で失敗しても ENAMETOOLONG、他の失敗は EIO。
- 失敗した時の出力は、引数が NULL でない限り常に NULL・0 に直す（引数の誤りの時も）。`wb_os_library_open` は names が NULL でも `*library` を NULL にする。
- `wb_os_library_open`: 空の名前は位置に関係なく先に全部検査して EINVAL。Windows では UTF-8 でない名前・長すぎる名前は読めない名前として次へ進み、全部だめなら ENOENT。
- `wb_os_utf8_from_wide`: ENAMETOOLONG と EILSEQ は先に見つかった方（先頭から順に走る）。`count` が `(size_t)-1` の時、high surrogate の次の unit が NUL なら EILSEQ（その NUL までは読む）。
- `wb_os_data_directory` の Windows: 最後の `\`（または `/`）で切って `\share` を足す。GetModuleFileNameW の buffer は 260 から 65536 unit まで倍にして広げ、それでも足りなければ EIO。UTF-8 にして長さが WB_OS_PATH_MAX を超えると EIO（header の戻り値の一覧に ENAMETOOLONG が無いため）。
- `wb_os_trust_roots`: 順は current user の ROOT、machine の ROOT、current user の Disallowed、machine の Disallowed。store を開けない時は無い store として飛ばす。列挙の終わりは last error が 0 か CRYPT_E_NOT_FOUND の時だけ正常で、他は EIO。DER が 0 byte の物と X509_ASN_ENCODING でない物は飛ばす（0x30 で始まるかは見ない）。
- `wb_os_thread_name` の POSIX: `WB_OS_THREAD_NAMES` の時だけ `_GNU_SOURCE` を（未定義なら）file の先頭で定義して `pthread_setname_np` を呼ぶ。Windows は名前を 15 byte に切ってから UTF-16 にし、`SetThreadDescription` を呼ぶたびに探す（状態を持たない）。
- 乱数の POSIX は 1 MiB ずつ read し、EINTR は再試行、read が 0 を返したら EIO。
- ENOMEM（`wb_os_aligned_alloc` が posix で）の試験は ASan の allocator が巨大な要求で abort するため posix では飛ばし、Windows の側だけ（`1 << 62`）。

header の不備の指摘: なし（止める必要は無かった）。

未実施: aarch64 Windows の実行（compile だけ）、実機の Windows、Disallowed の distrusted 1 の経路の確認（上記）、`libbrowser` の既存 code を新しい OS の層へ替える作業（p021）。Wine の prefix（`build/tmp/ws334-wine`）の Disallowed の store へ書き込みを試みたが、enumerate に現れないので影響は見えていない。
