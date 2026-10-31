<!-- awesome-plan project=zedbsd record=ws334-p007 -->

# ws334-p007 network: os-net

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-11-01 担当 Sonnet 5.5。cleared は Q1）。
- 依存: p005（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.4 の socket・名前の解決・待ち・起こしの行、§2.2 の O1〜O6・O8・O32・O33、WS327 の design §5.2（select と loopback の起こしの理由）。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libbrowser/base/os.h`、`userland/desktop/libbrowser/base/os-win32.h`、`userland/desktop/libbrowser/base/os-net.h`。それが include する header も読む。
4. 仕様: POSIX.1-2024 の `getaddrinfo`・`socket`・`connect`・`getsockopt(SO_ERROR)`・`send`・`recv`・`poll`・`pipe`・`fcntl`、Linux の `MSG_NOSIGNAL`・`SOCK_CLOEXEC`。Microsoft Learn の `WSAStartup`・`WSASocketW`（`WSA_FLAG_NO_HANDLE_INHERIT`）・`ioctlsocket(FIONBIO)`・`connect`・`select`（except の set で失敗した connect）・`send`・`recv`・`closesocket`・`getaddrinfo`・`WSAGetLastError`・`InitOnceExecuteOnce`。手元に置く文書は無い。

## 作る物

- `userland/desktop/libbrowser/base/os-net-posix.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libbrowser/base/os-net-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-os-net.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 試験は自分の listen の socket を system の API で直に作ってよい（`127.0.0.1` の port 0、echo の thread）。外の network を使わない。
- resolve: "127.0.0.1" と "80" → 1 個以上、"localhost"、"::1"（host に IPv6 が無い時は記録して飛ばす）、空・NULL → EINVAL、"no-such-host.invalid" → ENOENT か EAGAIN（network の無い host では EAGAIN もありうる）で `*addresses` NULL。`wb_os_addresses_count(NULL)` は 0、`wb_os_addresses_free(NULL)`。
- connect: 非 blocking で listen 中の port へ → 0 か EINPROGRESS、poll で WRITABLE、`connect_result` 0。 接続の途中で `connect_result` を呼ぶと EINPROGRESS（試験の listen の backlog を満たして接続を待たせるか、Windows は WSAEWOULDBLOCK の直後に呼ぶ）。Windows の非 blocking の connect の返りが EAGAIN でなく EINPROGRESS。閉じた port（bind して番号を得て閉じる）→ connect が ECONNREFUSED か、poll が WRITABLE（Windows は WRITABLE | FAILED）で `connect_result` が ECONNREFUSED。index が範囲の外 → EINVAL。blocking の connect と送り・受けの往復（echo）。
- send・receive: 0 byte の send、相手が読まない非 blocking の socket に送り続けて EAGAIN になる（sent の合計を記録）、何も来ていない非 blocking の receive → EAGAIN、相手の close の後 poll が READABLE で receive が 0 byte と 0、相手が閉じた後の send の繰り返しで EPIPE か ECONNRESET になり、SIGPIPE で落ちない（posix は SIGPIPE を既定のままにして試す）。
- `wb_os_socket_descriptor`: 正の int、NONE → EINVAL。`wb_os_socket_last_error`: posix は不正な descriptor への system の `recv` の後 EBADF か ENOTSOCK、win32 は閉じた SOCKET への `recv` の後 ENOTSOCK。
- poll: timeout 0 は ready 0、50 ms は試験の時計で 40 ms 以上待つ、NONE の item は found 0、count 0 と timeout 20 は眠って 0、count 0 と -1 → EINVAL、count が WB_OS_POLL_MAX + 1 → EINVAL、wants 0 → EINVAL、timeout -2 → EINVAL。100 個の waker の 100 item（Windows の FD_SETSIZE 64 を超える）で 1 つだけ鳴らすと ready 1 でその item だけ READABLE。
- waker: ring で READABLE、drain の後は READABLE でない、drain せず 100000 回 ring しても止まらない、別の thread の ring が `timeout -1` で待つ thread を起こす（10 回）、`wb_os_waker_close(NULL)`。
- 共通: NULL の引数 → EINVAL。ASan・UBSan の報告 0。

## 注意

- Windows Sockets の開始は header の通り once（`InitOnceExecuteOnce` か `pthread_once`）。失敗を覚えて以後も EIO。
- Windows の select の `fd_set` は count に合わせて確保する（`FD_SETSIZE` を超える。`fd_set` の形 = `u_int fd_count` と `SOCKET fd_array[]` を数に合わせて確保し、`FD_SET` の macro を使わない）。select の timeout は `struct timeval`。
- POSIX の send は Linux・zedBSD に `MSG_NOSIGNAL`、FreeBSD にもある。無い system の `SO_NOSIGPIPE` は compile の条件（`#ifdef MSG_NOSIGNAL`）で選んでよい（system の選びではなく機能の有無）。
- waker の Windows の側の accept の確かめ（相手の address と port が自分の connect の socket の `getsockname` と一致）を header の通りに。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-net` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
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

作った物: `os-net-posix.c`・`os-net-win32.c`（骨格を実装）、`plan/ws334/tests/unit/test-os-net.c`。header・他の module・Makefile・試験の道具は変えていない。

実行した command と最後の行（最終の値は Q1 への報告にも載せた）:

- `sh plan/ws334/tests/run-unit.sh os-net`: `test-os-net: PASS (1289 checks) (posix)`、`test-os-net: PASS (1280 checks) (win32)`、`run-unit: PASS`（win32 の件数が少ないのは Windows で飛ばす posix 専用の確かめがあるため。逆もある）。
- `sh plan/ws334/tests/run-unit.sh all`: 全 test が PASS。ただし `test-os-zones (win32)` が 1 回 "wine server seems to be running, but I cannot connect" で FAIL（他の担当の Wine との衝突）。`run-unit.sh os-zones` の再実行で両方 PASS（1293 checks）。
- `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh`: `check-skeleton: PASS`（headers 19、sources 22）。
- `git diff --check`: 出力なし。

わざと壊した確認（git の外で変えて元に戻し、`cmp` と `git diff --quiet` で一致を確認）: posix 17 通り、win32 10 通り。
posix: `MSG_NOSIGNAL` を 0 に、`connect_result` の ENOTCONN を 0 に、index の `>=` を `>` に、drain を 1 回で止める、poll の最大数を 1 ずらす、`*sent` を要求の長さにする、receive の ECONNRESET の写しを消す、poll の READABLE から HUP・ERR・NVAL を外す、FAILED を落とす、`SOCK_CLOEXEC` を外す、waker の pipe を blocking にする、`ERANGE` を外す、NONE の item の found を 0 に直さない、`EAI_NONAME` を EIO に、no-item の sleep を 0 にする、ring の write を 0 byte にする。
win32: `WSAEWOULDBLOCK` を EINPROGRESS にしない、except の set で FAILED を付けない、fd_set に 64 個までしか入れない、`SO_ERROR` の写しを EIO に、ring の send を 0 byte に、no-item の Sleep を外す、`WSA_FLAG_NO_HANDLE_INHERIT` を外す、`WSAHOST_NOT_FOUND` を ENOENT にしない、write の set の印を READABLE にする、receive の ECONNRESET の写しを消す。
全部 FAIL（exit 1 か、waker の壊しは 600 秒の timeout の 124）で検出。最初に見逃した 1 つ（poll の READABLE から HUP・ERR・NVAL を外す）は、閉じた descriptor を poll に入れて READABLE | FAILED を確かめる試験（posix のみ）を足して検出するようにした。
見逃しが残る物: waker の Windows 側の accept の確かめ（相手の address と port が自分の物かの比較）と TCP_NODELAY（試験から他の process を割り込ませられない・値を外から読んでいない）。

自分で決めた点（header が黙っている所）:

- resolve: port が空文字も EINVAL。host・port が UTF-8 でない（Windows で変換できない）時は EINVAL。`EAI_NODATA` は定義がある system だけ ENOENT。port が数字でない・範囲外などは header の通り EIO（EAI_SERVICE を EINVAL にはしない）。
- connect: socket 引数の NULL でも、それ以外の失敗でも、`*socket` は NONE にして返す。blocking の connect が signal で切れた時は、POSIX は poll で終わりまで待って `connect_result` で結果を返す。ENFILE は EMFILE に、ENOBUFS（socket の作成）は ENOMEM に写す。`blocking` は 0 か 0 以外。
- send の errno の写し: EAGAIN/EWOULDBLOCK→EAGAIN、EPIPE・ENOTCONN→EPIPE、ECONNRESET・ECONNABORTED はそのまま、ENOBUFS・ENOMEM→ENOBUFS、EBADF・ENOTSOCK・EFAULT・EINVAL→EINVAL、他は EIO。receive は EAGAIN、ECONNRESET・ECONNABORTED・ETIMEDOUT、EINVAL 系、他は EIO（ENOTCONN は EIO）。
- 1 回の send・receive は POSIX で SSIZE_MAX、Windows で INT_MAX byte に切る。失敗時に `*sent`・`*received` を 0 にする。
- connect_result の Windows: header の getpeername の確かめだけでは Wine が接続の途中でも getpeername を成功させるため、`SO_ERROR` が 0 の時に待ち時間 0 の select で書き込み可能かも見て、書き込み可能でなければ EINPROGRESS にした（実機の Windows でも書き込み可能になるのは接続が終わった時なので矛盾しない）。
- `wb_os_poll`: `ready` が NULL なら EINVAL。検査は全 item を先に見てから `found` を 0 にする（EINVAL の時は items に触れない）。`wants` の R・W 以外の bit は無視。NONE 以外で負の socket（POSIX で INT_MAX 超も）は EINVAL。POSIX は NONE を除いた item だけを poll に渡す（poll の nfds が RLIMIT_NOFILE を超えて EINVAL になるのを避ける）。found は、HUP・ERR・NVAL が来たら FAILED と、求めた R・W を両方付ける。Windows は write を求めた socket を except の set にも入れ、except に残った物に WRITABLE | FAILED を付ける。select の失敗は WSAEINTR→ready 0 で成功、WSA_NOT_ENOUGH_MEMORY・WSAENOBUFS→ENOMEM、他は EIO。item 数が 0（または全部 NONE）で timeout 0 は即 0、正は sleep。
- waker の Windows: 外側の socket は blocking の connect（backlog 1 の loopback）。accept の前に select で最長 5 秒待ち（待ちが切れたら EIO）、他人の接続は最大 8 回まで閉じて繰り返す。ring 側（connect した側）に TCP_NODELAY、wait 側（accept した側）に `SetHandleInformation` で継承を外す。drain は EAGAIN か 0 か失敗まで読み続ける。
- Windows Sockets の開始は `InitOnceExecuteOnce`、失敗は `os_net_start_status` に EIO を覚える。resolve・connect・connect_result・send・receive・poll（item が有る時）・waker_open が呼ぶ。`wb_os_socket_close`・`descriptor`・`last_error` は呼ばない。
- 試験の時間: 40 ms 以上待つ試験は 50 ms の poll、no-item の sleep は 20 ms の poll で 15 ms 以上。waker の別 thread の wake は、8 秒後に別の waker を鳴らす見張りを付けて、壊れていても hang しない（ただし ring 自体が壊れると見張りも鳴らないので 600 秒の timeout で FAIL になる）。

header の不備の指摘: なし。

未実施: aarch64 Windows の実行（compile だけ）、実機の Windows、他の process が割り込んだ時の waker の accept（外から割り込ませる試験を作っていない）、IPv6 のない host での `::1`（この host には有るので実行は有る側のみ）。Wine での connect の途中で getpeername が成功する動作は、実機の Windows と違う（上記の select の確かめで吸収）。
