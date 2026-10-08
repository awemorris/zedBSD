<!-- awesome-plan project=zedbsd record=ws130 -->

# WS130: IPv6 の network stack

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p002〜p007 cleared、p008 は T1-316 PASS で 5330 の DHCPv6 の UAT 待ち、p001（設計）は Q1 の判定（後続が実装済み））（2026-10-06: p001・p002 cleared（p002 は T1-206b PASS）、p003 cleared（T1-243）、次は p004 libc）
Primary Milestone: MG005
Related Milestones: MG002（POSIX の socket API）
Objectives: O1, O3
Parent: [Master](../master.md)
Queue: q746（p001 の設計、P1）
Resume point: p001（2026-10-05 P1 が設計の第 1 版を書いた。UAPI（H1）と H2〜H8 のユーザーの判断待ち。code は判断の後）
<!-- awesome-plan-current:end -->

## 目標（2026-10-03 ユーザー）

「WSを立てましょう。計画だけ行っていきます。ベータ2以降で実装します。dhcpc -6というコマンドを作るのがいいと思います。SLAACはどうすればいいかなあ。NDPもどうすれば。」

- 今の kernel の network stack（`src/kern/net/`）は IPv4 だけ（ARP・ICMP・IPv4・inet socket）。`AF_INET6` は `include/uapi/socket.h` に定数だけある。
- IPv6 の本体・ICMPv6・NDP・SLAAC・`AF_INET6` の socket と、userland（`dhcpc -6`、`getaddrinfo` の IPv6、ping・ifconfig・route の IPv6、networkd の IPv6 の route と DNS）を設計し、ベータ2 以降に Phase に分けて実装する。

## 決まっていること

- DHCPv6 は既存の `dhcpc` に `-6` を足す（ユーザー）。

## 決まったこと（2026-10-03 ユーザー）

- 「SLAACは、カーネルはRAを受け取る。networkdが解決する、でいいと思います。networkdはインタフェースを開いて通知を監視できますしね。」→ NDP（近隣の cache・NS/NA・DAD・到達性・redirect）は kernel、RA は kernel が受けて networkd へ通知し、SLAAC の address・route・DNS（RDNSS）は networkd が決めて設定する。
- 「IPv6について、net.confの記法を ipv4: と ipv6: に分離する必要がありますね。個別に無効にする設定も。」→ `net.conf` の interface ごとの設定を `ipv4:` と `ipv6:` の節に分け、それぞれを個別に無効にできる記法にする（既存の IPv4 の記法からの移行と互換を p001 で設計）。
- address の方式（2026-10-03 user「RFC 7217, RFC 8981は両方使いましょう。記録しておいてください。」で決定）: 主の address は RFC 7217 の安定な address（MAC を出さず、同じ network では同じ値）、外への接続には RFC 8981 の一時的な address を既定で有効（macOS・Windows・最近の Linux と同じ）。どちらも `net.conf` の `ipv6:` で切り替えられる。

## 設計の論点（p001 で選択肢を出してユーザーと決める）

- **NDP**: 近隣の cache・NS/NA・DAD・到達性の確認・redirect を、ARP と同じく kernel に置く案（BSD・Linux と同じ）。
- **SLAAC**: (1) kernel が RA を受けて address を作る（Linux の既定）、(2) kernel は RA を受けて userland に渡し、networkd が address・route・DNS（RDNSS）の方針を決めて設定する（BSD の rtsold に近い、O3 の networkd に一貫）。privacy address（RFC 8981）、stable な address（RFC 7217）の要否。
- DHCPv6 と RA の M/O の flag の扱い、DNS は RDNSS（RA）と DHCPv6 のどちらを優先するか。
- dual stack での route と resolver の決め方（ws005-p019 の有線優先の仕組みとの整合）。
- POSIX の `<netinet/in.h>`・`getaddrinfo`・`inet_pton` の IPv6 の範囲（WS001 の台帳）。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（kernel と userland の境界、UAPI の案、`net.conf`、段、試験の方法） | planning（2026-10-05 第 1 版、H1〜H8 待ち） | ユーザーとの議論 |
| [p002](phase002/phase.md) | kernel の核（UAPI、`ipv6.c`・ICMPv6・NDP・DAD・MLDv2・address と route の表、RS・RA と route socket） | cleared（2026-10-06、T1-206b PASS） | p001（H1〜H4・H6、ユーザー承認 2026-10-05） |
| [p003](phase003/phase.md) | transport（address を 16 byte に、`AF_INET6` の UDP・TCP・ICMPv6、`IPV6_V6ONLY`、PMTU、source の選択） | cleared（2026-10-06、T1-243） | p002、H2 |
| [p004](phase004/phase.md) | libc（`inet_pton`・`inet_ntop`・`getaddrinfo`・`getnameinfo`・resolver） | cleared（2026-10-07、T1-286） | p003 |
| [p005](phase005/phase.md) | 道具と `net.conf` の `ipv6:`（`net`・`ifconfig`・`route`・`ping`・`host`・`nslookup`） | cleared（2026-10-07、T1-290） | p003、p004、H3 |
| [p006](phase006/phase.md) | networkd の SLAAC（link-local・RFC 7217・RFC 8981・RA・既定の route・RDNSS・DNSSL） | cleared（2026-10-07、T1-296） | p002、p005、H4・H5 |
| [p007](phase007/phase.md) | `dhcpc -6`（stateless・stateful、DUID、Renew） | cleared（2026-10-07、T1-300） | p006、H7 |
| [p008](phase008/phase.md) | T1（slirp と tap+netns の dnsmasq を 1 つの QEMU で）と 5330 の UAT | QEMU は T1-316 PASS、5330 の UAT は保留 | p007 |
| p009 | 全文規約の見直し | planning | p008 |
