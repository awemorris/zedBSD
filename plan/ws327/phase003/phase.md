<!-- awesome-plan project=zedbsd record=ws327-p003 -->

# ws327-p003 network の層（`keiland_socket.h`、KL_VERSION 104）と host 試験

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §4。Queue: q1697 / q1697-i01（担当 P2、2026-10-30）。
- Status: in-progress（2026-10-30。host 試験と zedBSD 3 arch の build は済み。受け入れの zedBSD の guest での実行（design §8）は未実施）
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
- 未実施: zedBSD の guest での thread-test・socket-test（受け入れの条件）、FreeBSD の build と試験。
