<!-- awesome-plan project=zedbsd record=ws327-p014 -->

# ws327-p014 network の層の Win32 の実装（`socket-win32.c`）と mingw の cross compile

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §4・§5.2・§5.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007（p013 と並べてよい）。
- Status: in-progress（2026-10-30。実装と確かめは済んだ、cleared の判定は Q1）
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
| `plan/ws327/phase014/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

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
- **close**: `kl_socket_close(NULL)`・`kl_socket_waker_close(NULL)` は何もしない（POSIX の実装と同じ。Mailer の `ml_conn_close` が開けな
  かった接続で NULL を渡す。design-reviewer #9）。
- **waker**: open は骨格の comment の手順（`SO_EXCLUSIVEADDRUSE` の listen、`getsockname`、blocking の connect、accept の peer の照合、
  listen の close、両端の非 blocking、`TCP_NODELAY`）。ring は `InterlockedCompareExchange(&rung, 1, 0) == 0` の時だけ 1 byte を `send`。
- **誤りの表**: `socket_error`（§5.3 と骨格の comment の全て）。`EHOSTDOWN`・`ENONET` は mingw の `errno.h` に無い（`#if defined` で）。
  EAGAIN だけを返す（EWOULDBLOCK は mingw で別の値 140）。
- **resolve**: `GetAddrInfoW`（`ws2tcpip.h`）、UTF-8 → UTF-16、hints の family・socktype、`AI_ADDRCONFIG` 無し、答えを
  `socket_from_system` で、`capacity` まで、`FreeAddrInfoW`。誤りは `socket_resolve_error`。
- **address**: `inet_pton`・`inet_ntop`（ANSI、Vista 以降）、"%scope" は数字か `if_nametoindex`（`netioapi.h`、`-liphlpapi`）。
  format は POSIX と同じ文字列（"[v6%scope]:port"）。any・loopback は system を呼ばない。

## 4. 確かめの command

1. `sh plan/ws327/tests/build-win32.sh` → x86_64 と i686 の両方で `compile socket-win32.c: PASS (warnings 0)`・`link win32-link.exe: PASS`、
   最後に `ws327-win32: PASS`。
2. `sh plan/ws327/tests/check-base.sh p014` → PASS。
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
plan/ws327/phase014/phase.md`（ws.md は Q1）。動作の確かめはしない（決定 3）。

## 結果

2026-10-30（host の `date`）。担当: 実装の担当（Sonnet 5.5 high）。動作は走らせていない（決定 3）。

### 流した command と最後の行

- `sh plan/ws327/tests/build-win32.sh` → x86_64・i686 とも `compile socket-win32.c: PASS (warnings 0)`、`link win32-link.exe: PASS`、最後 `ws327-win32: PASS`。
- `sh plan/ws327/tests/check-base.sh p014` → `ws327-check-base: PASS`。
- `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p014-linux build/ws327-p014-linux/lib/libkeiland.so` → 終了 0、warning 0。
- `git diff --check` → 0。
- 突然変異（§5）: `kl_socket_waker_ring` を消す → 両方で `link win32-link.exe: FAIL`。使わない局所の変数 `unused_mutation` を足す → 両方で `compile socket-win32.c: FAIL`。
  どちらも元に戻して `cmp` で一致を確かめた。

### 自分で決めた点（phase.md と違う所・足した所）

- `struct kl_socket` に `connecting` を足した（connect が EINPROGRESS を返してから `connect_result` までの間 1）。`WSAEINVAL` を EALREADY にするのは
  `nonblocking && connecting` の時だけ。これが無いと、listen の socket への connect（`WSAEINVAL`）も非 blocking なら EALREADY になる。
- `kl_socket_poll` の 3 つの集合は 1 回の `malloc(3 * set_bytes)` を切り分ける（`offsetof(fd_set, fd_array) + count * sizeof(SOCKET)` の倍数で、
  `SOCKET` の整列に合う）。`count` が大きすぎて掛け算が溢れる時は ENOMEM。
- `kl_socket_resolve` は service 名を渡さず（`GetAddrInfoW(name, NULL, ...)`）、答えの port を後で入れる（port の文字列の変換を避けた）。
- accept の取り直しは `WSAECONNRESET` に加えて `WSAECONNABORTED`（design §4.3 の ECONNABORTED）。POSIX の `ENETDOWN` などの取り直しは Windows の
  accept の誤りに無く、`WSAENETDOWN` を取り直すと blocking の accept が回り続けるので取り直さない。
- `kl_socket_receive` は `WSAEMSGSIZE`（datagram socket の接続済みの recv が長い datagram を切った）を buffer の大きさで成功、`WSAESHUTDOWN`
  （`shutdown(SD_RECEIVE)` の後）を `*received == 0`（終わり）で成功にした（POSIX は両方とも誤りにならない）。
- `socket_error` に表へ足した値: `WSAEMSGSIZE` → EMSGSIZE、`WSAENOTSOCK` → ENOTSOCK、`WSAEOPNOTSUPP` → EOPNOTSUPP、`WSA_NOT_ENOUGH_MEMORY` → ENOMEM。
  `SetHandleInformation` の失敗は EIO。
- 全ての公開の関数（address の 4 つと `kl_socket_native`・`kl_socket_close(NULL)`・`kl_socket_waker_close(NULL)` を除く）が最初に `socket_start()`。
  `socket_start` が失敗すれば void の関数（`ring`）は何もしない。
- waker の組は accept の peer（accept が書く address）を connect した側の `getsockname` と比べる（phase.md の `getpeername` と同じ値）。
  listen の backlog は 4。ends[0] が accept した側（読む）、ends[1] が connect した側（書く）。

### Q1 へ（Windows で header の約束をそのまま守れない所・差）

Q1 の決定（2026-10-30）: 1 の send の ECONNABORTED・ECONNRESET はそのまま（header は変えない）、3 は POSIX に合わせる（下に反映）。

header は変えていない。次は「Windows では約束の文言のとおりにならない」所で、いずれも header の「or another errno value」や Mailer の使い方
（EPIPE と ECONNRESET を同じに扱う、`ml_conn_*`・sync.c）の範囲に収まる。実機では確かめていない。

1. **send の EPIPE**: header「peer が閉じた socket への send は EPIPE」。WinSock は自分が `shutdown(SD_SEND)` した後だけ `WSAESHUTDOWN`（→ EPIPE）で、
   peer が閉じた（RST を受けた）後の send は `WSAECONNABORTED` か `WSAECONNRESET`（→ ECONNABORTED か ECONNRESET）。EPIPE にはならない。
   `socket_error` の表のとおりに返した。header の文言を「EPIPE か ECONNRESET か ECONNABORTED」にするか、send で `WSAECONNABORTED` を EPIPE に写すかは Q1 の判断。
2. **接続済みの UDP の ICMP 到達不能**: `SIO_UDP_CONNRESET` を切ったので、繋いでいない UDP の受信が `WSAECONNRESET` で壊れるのは避けられるが、
   接続した UDP の受信が Linux のように ECONNREFUSED を返す事も無くなる（Windows には同じ通知が無い）。header は約束していない。
3. **bind していない socket の `kl_socket_local_address`**: Windows の `getsockname` は `WSAEINVAL` だが、Q1 の決定（2026-10-30）で POSIX に合わせ、
   `WSAEINVAL` なら 0 を返して socket の family の any の address・port 0 にした（`kl_address_any`）。差は無くなった。
4. **bind の乗っ取り**: `SO_EXCLUSIVEADDRUSE` を付けない（design §5.2）ので、Windows では他の process が `SO_REUSEADDR` で同じ port に bind できる
   （listen する server 用途の app が出たら再検討）。waker の listen の socket だけは `SO_EXCLUSIVEADDRUSE` で、accept の peer を照合する。
5. **ready の意味**: POSIX の `POLLHUP`（両方向が閉じた → events の全て）は select に無い。Windows の select は FIN を受けた socket を readable、
   RST の socket を readable と writable に出すが、「閉じた socket の WRITABLE」は実機で未確認。FAILED は connect 失敗を except 集合で出す事で守れる。
6. **link**: 利用側（libkeiland の Windows 用の Makefile、本 WS の範囲の外）は `-lws2_32 -liphlpapi`（`if_nametoindex`）が要る。
   `inet_pton`・`inet_ntop` は Vista 以降、`WSA_FLAG_NO_HANDLE_INHERIT` は Windows 7 SP1 以降、`SetThreadDescription` 側の thread-win32.c と
   合わせて Windows 10 を前提にした（build-win32.sh は `_WIN32_WINNT=0x0A00`）。

### 未実施

- 走らせていない（Windows でも wine でも。決定 3）。poll の select の写し・waker の組・SIO_UDP_CONNRESET・accept の取り直しは build が通るだけで動作は未確認。
- 規約の全文の見直しは自分の diff を `plan/coding-style.md` §14 で読んだ範囲（forward 宣言・関数の comment・宣言の位置・paragraph の comment・
  条件の中で関数を呼ばない・最後の return は成功）。
