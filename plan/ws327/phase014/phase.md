<!-- awesome-plan project=zedbsd record=ws327-p014 -->

# ws327-p014 network の層の Win32 の実装（`socket-win32.c`）と mingw の cross compile

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §4・§5.2・§5.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007（p013 と並べてよい）。
- Status: planned
- Disposition: normal
- ユーザーの決定 3（ws.md、原文）: 「P2ですが、Windows用の実装を書いてしまってOKです。インタフェースがWindowsでも通用する形になっているかのチェックです。動作確認は不要です。mingwは使ってよいです。」
  → mingw で compile と link（warning 0）まで。wine でも Windows でも走らせない。

## 1. 読む物

- `AGENTS.md` §4・§7・§10、`plan/coding-style.md` の全文。
- 仕様: `userland/desktop/include/keiland/keiland_socket.h`（各関数の約束。変えない）、design.md §4.3（約束の詳しい形: poll の ready の
  意味、waker の順、accept の取り直し、send・receive の EAGAIN、datagram の切り）・§5.2（Win32 への写しの表と poll の写し）・§5.3（誤りの表）。
- 骨格: `userland/desktop/libkeiland/platform/socket-win32.c`（P2 が作った。各関数の comment が Win32 での作り方）。
- 手本（POSIX の実装、変えない）: `userland/desktop/libkeiland/platform/socket-posix.c`。引数の確かめ（EINVAL の条件）・address の写し・
  poll の ready の作り方・waker の順は同じにする。
- Win32 の API の宣言: host の `/usr/x86_64-w64-mingw32/include/`（`winsock2.h`・`ws2tcpip.h`・`mswsock.h`・`mstcpip.h`・`netioapi.h`）。
- design.md §13（共通の約束。この phase.md の一部として従う）。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/libkeiland/platform/socket-win32.c` | 全ての関数を骨格の comment どおりに書く。static の補助を足してよい。record の型は変えてよいが、変えた点を結果に書く |
| `plan/ws327/phase014/phase.md` の「結果」、`plan/ws327/ws.md` の p014 の行 | 記録 |

libkeiland の Makefile には足さない。`thread-win32.c` は p013 の物で触らない（`kl_once` はそこの関数を使う。p013 の前でも link は通る）。

## 3. 作り方の要点（骨格の comment の要約。食い違えば header が正）

- **WinSock の始め**: 全ての公開の関数（address の 4 つの純粋な関数を除く）の最初で `socket_start()`。`kl_once(&socket_started, socket_start_once)`
  の中で `WSAStartup(MAKEWORD(2, 2), &data)`、0 でなければ `socket_start_error` に（`WSASYSNOTREADY`・`WSAVERNOTSUPPORTED` などは EIO）。
- **引数の確かめ**: POSIX の実装と同じ EINVAL・EAFNOSUPPORT の条件（family・kind・flags の範囲、NULL の引数）。
- **open**: `WSASocketW(domain, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)`、`INVALID_SOCKET` は
  `socket_error(WSAGetLastError())`。非 blocking は `ioctlsocket(FIONBIO, 1)`。IPv6 は `setsockopt(IPPROTO_IPV6, IPV6_V6ONLY, 1)`（DWORD）。
  UDP は `WSAIoctl(SIO_UDP_CONNRESET, FALSE)`（`mswsock.h`）。途中の失敗は `closesocket` して誤り。
- **connect**: `WSAEWOULDBLOCK` は EINPROGRESS。2 度目は `WSAEALREADY`（と古い WinSock の `WSAEINVAL`、非 blocking の時だけ）を EALREADY。
  `WSAEISCONN` は 0 にせず EISCONN（POSIX と同じ）。
- **accept**: `accept`、`WSAECONNRESET` は取り直し（非 blocking で次が無ければ `WSAEWOULDBLOCK` → EAGAIN）、新しい socket を
  flags の非 blocking に、`SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0)`、record（family は listen の socket の物）、peer。
- **send・receive**: 大きさを `INT_MAX` で切る。`send` の `WSAESHUTDOWN` は EPIPE、`WSAEWOULDBLOCK` は EAGAIN。`recvfrom` の
  `WSAEMSGSIZE` は buffer の大きさで成功。`sendto` の大きさが `INT_MAX` を超えれば EINVAL（datagram は切らない）。
- **poll**: §5.2 の select の写し。3 つの集合を `offsetof(fd_set, fd_array) + count * sizeof(SOCKET)` で malloc（ENOMEM）、`fd_count` を
  数えて入れる。`count == 0` は `Sleep`（FOREVER は EINVAL、負は -1 の他 EINVAL）。`count` が 1 以上なら集合に必ず socket が 1 つ以上入る（waker も
  socket の組）ので select の WSAEINVAL は起きない。時間は `struct timeval`（FOREVER は NULL）。select の後: read の集合に有れば READABLE（events に有る時）、
  write は WRITABLE、except は FAILED（events に関わらず）。waker は read に有れば `recv` で読み切り（`WSAEWOULDBLOCK` まで）、その後
  `InterlockedExchange(&rung, 0)`、READABLE。`*ready` は ready が 0 でない wait の数。集合を free。
- **waker**: open は骨格の comment の手順（`SO_EXCLUSIVEADDRUSE` の listen、`getsockname`、blocking の connect、accept の peer の照合、
  listen の close、両端の非 blocking、`TCP_NODELAY`）。ring は `InterlockedCompareExchange(&rung, 1, 0) == 0` の時だけ 1 byte を `send`。
- **誤りの表**: `socket_error`（§5.3 と骨格の comment の全て）。`EHOSTDOWN`・`ENONET` は mingw の `errno.h` に無い（`#if defined` で）。
  EAGAIN だけを返す（EWOULDBLOCK は mingw で別の値 140）。
- **resolve**: `GetAddrInfoW`（`ws2tcpip.h`）、UTF-8 → UTF-16、hints の family・socktype、`AI_ADDRCONFIG` 無し、答えを
  `socket_from_system` で、`capacity` まで、`FreeAddrInfoW`。誤りは `socket_resolve_error`。
- **address**: `inet_pton`・`inet_ntop`（ANSI、Vista 以降）、"%scope" は数字か `if_nametoindex`（`netioapi.h`、`-liphlpapi`）。
  format は POSIX と同じ文字列（"[v6%scope]:port"）。any・loopback は system を呼ばない。

## 4. 確かめの command

1. `sh plan/ws327/tests/build-win32.sh` → `compile socket-win32.c: PASS (warnings 0)`・`link win32-link.exe: PASS`・`ws327-win32: PASS`。
2. `sh plan/ws327/tests/check-base.sh` → PASS。
3. `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p014-linux build/ws327-p014-linux/lib/libkeiland.so` → 0。

## 5. 突然変異の確かめ（§13.6 の Win32 の形）

1. 関数を 1 つ消す（例: `kl_socket_waker_ring`）→ link が FAIL。
2. 使わない局所の変数を 1 つ足す → compile が FAIL。
3. 戻して `cmp`。

## 6. 注意

- 引数の名前 `socket` は関数 `socket()` を隠す: socket を作るのは `WSASocketW` で、`socket(` を呼ばない。
- `fd_set` の配列を malloc で大きく取る時、`FD_SET` の macro（`FD_SETSIZE` で止まる）を使わず `fd_count` と `fd_array` を直に書く。
- `SOCKET` は `UINT_PTR`: `INVALID_SOCKET` と比べる（-1 と比べない）。`kl_socket_native` は `(intptr_t)handle`。
- `WSAPoll` を使わない（Windows 10 2004 より前の不具合、§5.2）。
- waker の「読み切ってから印を下ろす」の順を守る（逆は鳴りが永久に失われる、design §11 の 1）。
- 写せない所・header の約束を Win32 で守れない所は、直さずに結果の「Q1 へ」に書く（ユーザーの決定 3 の目的）。
- source の comment に開発の管理情報を書かない。「Not written yet.」の comment は中身の comment に置き換える。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜3 が PASS、§5 の 1・2 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/libkeiland/platform/socket-win32.c
plan/ws327/phase014/phase.md plan/ws327/ws.md`。動作の確かめはしない（決定 3）。

## 結果

（実装の担当が書く）
