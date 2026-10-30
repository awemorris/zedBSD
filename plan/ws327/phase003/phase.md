<!-- awesome-plan project=zedbsd record=ws327-p003 -->

# ws327-p003 network の層（`keiland_socket.h`、KL_VERSION 104）と host 試験

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §4。Queue: q1697 / q1697-i01（担当 P2、2026-10-30）。
- Status: cleared（2026-10-30 Q1 の判定、ws.md の表）
- Disposition: normal

## 目的・受け入れ

- `kl_address_*`・`kl_socket_*`・`kl_socket_poll`・waker（POSIX、BSD socket と `poll`）。build warning 0（zedBSD 3 arch・Linux）、host 試験
  （loopback、IPv4・IPv6、poll、非 blocking の connect、TSan）、thread-test・socket-test を zedBSD の guest で 1 回（review 11）。

## 実装

- `userland/desktop/include/keiland/keiland_socket.h`（新）、`keiland.h` が include、`keiland_version.h` 104（Q1 が割り当て）、`exports.map` 再生成。
- `userland/desktop/libkeiland/platform/socket-posix.c`（新、3 つの Makefile に追加）: `SOCK_CLOEXEC`、IPv6 は `IPV6_V6ONLY` 1、`SO_NOSIGPIPE` の
  有る system は付ける、send は `MSG_NOSIGNAL`、EINTR は中で繰り返す、EWOULDBLOCK は EAGAIN に、accept は `accept4` で flags を揃え消えた接続を
  飛ばす、poll は 32 までは stack の `pollfd`、ready は events との積と FAILED、waker は `pipe2` と印（読み切ってから印を下ろす）。

## 確かめ（2026-10-30）

- host: `sh plan/ws327/tests/host-test.sh build/ws327-linux` → thread・socket × gcc・clang × （lib・TSan・ASan+UBSan）の 12 build 全て PASS。
  socket-test.c 53 項目（address 13、resolve 5、TCP の往復 IPv4・IPv6、非 blocking の connect の成功 IPv4・IPv6 と拒否、accept の flags 4、
  16 socket の poll と時間切れ、waker 5（他の thread の ring で FOREVER の poll が返る、合流、静けさ、競合、`waker-no-lost-ring`）、相手の close の
  受けの 0 と send の EPIPE（SIGPIPE で死なない）、UDP IPv4・IPv6 と切り詰め、IPV6_ONLY の既定と変更、option の誤り、poll の限界 4）。
- 逆の順の waker（印を下ろしてから読み切る）の mutant を作り、`waker-race`・`waker-no-lost-ring` が 3 回とも FAIL する事を確かめた（試験が
  review 1 の誤りを見つける）。
- build: Linux warning 0。zedBSD amd64・arm64・riscv64 の libkeiland は make 0・warning 0（`sh plan/ws327/tests/build-zedbsd.sh p003 ARCH ""`、
  `build/ws327-p003-*.log`）。
- zedBSD の guest（2026-10-30 09:14、q1697-i02）: image `plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-layers.mk
  build/ws327-layers-img`（userland の基本と libkeiland と SSH、GPU 無し。Mailer は p008 まで build しないので
  `config-amd64-guest.mk` は使わない）、`sh plan/ws327/tests/guest-layers.sh build/ws327-p003-guest/run4 build/ws327-layers-img`
  （QEMU+KVM、GUEST_RUNTIME は run の directory、SSH で put・run・cat。console は読まない）→ `ws327-guest-layers: PASS`:
  thread-test 27 PASS（名前の読み返しは Linux だけ）、socket-test 53 PASS・FAIL 0・NOT-DONE 0、guest は fb2d34d の zedBSD。
  - 1 回目（run1）: thread-test.c の Linux だけの変数が zedBSD で unused（-Werror）→ `#if defined(__linux__)` に入れた。
  - 2・3 回目: `resolve-localhost` が ENOENT（zedBSD の errno 6）。zedBSD の image に `/etc/hosts` が無く、DNS（slirp）も
    "localhost" を答えない（libc の resolver は数字 → /etc/hosts → DNS）。層は getaddrinfo の EAI_NONAME を ENOENT にして正しい。
    guest-layers.sh が捨てる guest の `/etc/hosts` に loopback の 2 行を足し（無い時だけ）、socket-test は失敗の時に答えを
    `NOTE` で書く。**Q1 へ: zedBSD の image に `/etc/hosts`（localhost）が無い事は WS327 の外の所見**（app が "localhost" を
    解けない。Bug にするかは Q1）。
  - host の再試験（試験の file を直した後）: `sh plan/ws327/tests/host-test.sh build/ws327-linux` → 12 build PASS（thread 28、socket 53）。
- 未実施: FreeBSD の build と試験（design §8 の FreeBSD の guest。p016 の受け入れで）。
