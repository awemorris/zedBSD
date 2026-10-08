<!-- awesome-plan project=zedbsd record=ws130-p001 -->

# ws130-p001: IPv6 の設計（kernel と userland の境界、UAPI の案、段、試験）

Phase ID: `ws130-p001`
Parent: [WS130](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 後続の p002〜p007 が実装・cleared）（旧: planning（2026-10-05 P1 generation17。設計の第 1 版。code は §9 の判断（UAPI を含む）の後））
Phase disposition: normal
Queue: q746（ベータ2 の P1 の列の 9 番目）

## 範囲

- ユーザー（2026-10-03、原文）:「WSを立てましょう。計画だけ行っていきます。ベータ2以降で実装します。dhcpc -6というコマンドを作るのがいいと思います。SLAACはどうすればいいかなあ。NDPもどうすれば。」
- 決まったこと（[ws.md](../ws.md)）: NDP は kernel、RA は kernel が受けて networkd に通知し、SLAAC の address・route・DNS は networkd が決める。
  `net.conf` は `ipv4:` と `ipv6:` を分けて個別に無効にできる。主の address は RFC 7217、外への接続は RFC 8981 の一時的な address（既定で有効）。DHCPv6 は `dhcpc -6`。
- ここで決めること: kernel の構造、UAPI の案、networkd・dhcpc・libc・道具の分担、`net.conf` の記法、段、試験の方法。
- 範囲外: 実装（p002〜）。router の機能（RA の送出、DHCPv6 の prefix delegation、転送）。IPsec。Settings の頁の表示（WS089、P2 と調整）。

## 1. 今の形（2026-10-05 の main を読んだ）

| 項目 | 今 | 場所 |
| --- | --- | --- |
| kernel の network | IPv4 だけ。Ethernet の type ごとに入力を登録（IPv4・ARP）。IPv4 は option・fragment を落とす（再構成なし）。interface ごとに IPv4 の address は 1 つ（`inet-socket.c` の表）。route は `uint32_t` の network・mask・gateway | `src/kern/net/`（`ipv4.c`・`arp.c`・`icmp.c`・`route.c`・`inet-socket.c`） |
| transport | TCP・UDP の endpoint・照合・checksum が `uint32_t` の address を前提（`tcp.c` 2,963 行、`udp.c`） | `src/kern/net/tcp.c`・`udp.c`、`include/kern/net/inet-socket.h` |
| UAPI | `AF_INET6`（24）・`struct sockaddr_in6`・`struct in6_addr`・`in6addr_any`・`IPPROTO_IPV6` は在る（socket は EAFNOSUPPORT）。`struct sockaddr` は 16 byte（`ifreq`・`rtentry` は `sockaddr_in6`（28 byte）を入れられない）。`sockaddr_storage` は 128 byte | `include/uapi/socket.h`・`netinet.h`・`netif.h`・`route.h` |
| 経路の socket | `AF_ROUTE` は読むだけの事象の socket（`RTM_IFINFO`: carrier・到着・削除） | `src/kern/net/route-socket.c` |
| 多 cast の受信 | USB の ECM・NCM は ALL_MULTICAST、AX211 は MCAST_FILTER の pass_all。IPv6 の solicited-node の group は filter なしで届く | `usb-cdc-ecm.c`、`usb-cdc-ncm-net.c`、`intel-ax211-assoc.c` |
| userland | `dhcpc`（IPv4、UDP socket、1 回で lease を取って address・route・resolver を書く、更新なし）。networkd が `dhcpc` を走らせ、有線の優先で既定の route を 1 つにする。`net`（oneshot）が `net.conf` を当てる | `userland/base/dhcpc/`・`networkd/`・`net/` |
| `net.conf` | interface ごとに `ipv4:`（`dhcp`・`dhcp-timeout`・`addresses`）は既に節。`routes`・`dns` は全体で IPv4 だけ。Wi-Fi は `net.conf` の外（wifi-store） | `userland/base/net/netconf.c`・`.h`、`userland/base/etc/net.conf` |
| libc | `inet_pton`・`getaddrinfo`・resolver（`resolv.conf` の nameserver）・`getnameinfo` は IPv4 だけ | `userland/base/libc/socket.c`・`resolver.c`・`resolv.c` |
| 道具 | `ifconfig`・`route`・`ping`・`host`・`nslookup`・`fetch`・`ntpdate` は IPv4 だけ | `userland/base/*` |
| QEMU | `-netdev user`（libslirp 4.8.0、QEMU 10.0.11）は IPv6 が既定で有効: RA（prefix `fec0::/64`）、DHCPv6 の information-request、host `fec0::2`、DNS `fec0::3` | `plan/tools/guest/guest.py` |

## 2. 全体の分担

| 層 | 受け持ち |
| --- | --- |
| kernel | IPv6 の入出力・拡張 header、ICMPv6（echo・error・Packet Too Big）、NDP（近隣の cache・NS/NA・NUD・DAD・redirect）、MLDv2（自分の group の報告）、RS の送出と RA の検査、address の表（状態と寿命）、IPv6 の route の表、source address の選択（RFC 6724）、`AF_INET6` の TCP・UDP・ICMPv6 の socket |
| networkd | RA の通知を受けて SLAAC（RFC 7217 の安定な address、RFC 8981 の一時的な address、link-local）、既定の route（有線の優先と同じ方針）、RDNSS・DNSSL（RFC 8106）を resolver に、M/O の flag で `dhcpc -6` を走らせる。DAD の失敗で作り直す |
| `dhcpc -6` | DHCPv6 の stateless（Information-Request: DNS）と stateful（IA_NA）。IPv4 と同じく 1 回で取って書く、更新は networkd が走らせ直す |
| `net` | `net.conf` の静的な IPv6 の address・route を当てる |
| libc | `inet_pton`・`inet_ntop`（RFC 5952 の正規の表記）、`getaddrinfo`（AAAA、RFC 6724 の並べ方、`AI_ADDRCONFIG`）、`getnameinfo`、resolver の IPv6 の nameserver |

## 3. kernel

### 3.1 address の表現と transport

- 案 A（推奨）: kernel の中の address を 16 byte の `struct in6_addr` に揃え、IPv4 は v4-mapped（`::ffff:a.b.c.d`）で持つ。TCP・UDP の endpoint・照合・pseudo header の
  checksum を 16 byte にし、出力は address の種類で `ipv4_output` か `ipv6_output` に分ける。dual stack の socket（`IPV6_V6ONLY` = 0）が自然に書ける。
- 案 B: TCP・UDP を IPv6 用に別に持つ。`tcp.c` の 3,000 行が 2 つになり、直しが 2 回要る。採らない。
- 案 A の段取り: p003 の最初で、振る舞いを変えずに IPv4 の address を 16 byte に替える（IPv4 の回帰は T1 の既存の network の試験）。

### 3.2 IPv6 の入出力（`ipv6.c`）

- 入力: version・payload の長さ・hop limit を確かめ、拡張 header を辿る（hop-by-hop: Router Alert（MLD）だけ見る、routing type 0 は落とす、destination options、
  fragment は**落とす**（IPv4 と同じく再構成しない、§9 H6）、未知の next header は ICMPv6 Parameter Problem）。宛先が自分の address・参加している多 cast でなければ落とす（転送しない）。
- 出力: route を引いて次の hop（on-link なら宛先、そうでなければ gateway）を NDP で解決し、MTU を超えれば EMSGSIZE（fragment を作らない）。
- Packet Too Big: 宛先ごとの path MTU の小さな cache（寿命 10 分）。TCP は MSS を下げて送り直す（IPv6 の router は fragment しないので、PPPoE・tunnel の回線で要る）。

### 3.3 NDP（`nd6.c`）

- 近隣の cache: 状態 INCOMPLETE・REACHABLE・STALE・DELAY・PROBE（RFC 4861）。解決待ちの packet は 1 つ保つ（ARP と同じ）。cache の大きさは ARP と同じ程度（interface あたり 64）。
- NS/NA の送受、DAD（address を tentative で入れ、NS を 1 回、1 秒で衝突が無ければ preferred）。衝突は route socket で通知（§3.6）。
- redirect は on-link の宛先の host route（寿命つき）として受ける。router の到達性の失敗で別の router に替えるのは networkd の仕事（v1 は NUD の失敗を通知するだけ）。
- RS: interface の link-local の DAD が済んだ時と carrier が上がった時に、最大 3 回・4 秒おき。RA は hop limit 255・link-local の送り元・checksum を確かめ、
  link の値（cur hop limit・reachable time・retrans timer・MTU option）を当て、送り元の近隣を STALE で入れ、本体を route socket で networkd に渡す。prefix・router・DNS は kernel が決めない。

### 3.4 MLDv2（`mld6.c`）

- 参加している group（all-nodes 以外: solicited-node・socket が join した group）の report を join の時に送り、query に答える。MLD snooping の switch では
  report が無いと solicited-node の NS が届かず、DAD と address の解決が壊れるため、v1 に入れる。

### 3.5 address・route の表

- interface ごとに address を最大 16（link-local・安定・一時的・DHCPv6・静的）。各 address は prefix 長・flag（tentative・deprecated・temporary・autoconf・dhcp・static）・
  valid と preferred の寿命（kernel が数えて、preferred が切れたら deprecated、valid が切れたら消して通知）。networkd が RA のたびに寿命を更新する。
- loopback は `::1/128`。interface ごとの IPv6 の有効・無効（無効なら address を持たず IPv6 の入力を落とす）。
- route の表: 宛先・prefix 長・gateway（link-local の gateway は interface で区別）・interface・metric・寿命（RA の router lifetime・Route Information）。最長一致、同じなら metric。
- source address の選択（RFC 6724）: 同じ scope、deprecated を避ける、一時的を優先（RFC 8981 の既定）、出口の interface、最長一致。

### 3.6 UAPI の案（**ユーザーの承認が要る**、§9 H1。正確な差分は p002 の最初に出す）

| 追加 | 内容 |
| --- | --- |
| `netinet.h` | `IPPROTO_ICMPV6 58`、`IPV6_V6ONLY`、`IPV6_UNICAST_HOPS`、`IPV6_MULTICAST_IF`・`_HOPS`・`_LOOP`、`IPV6_JOIN_GROUP`・`IPV6_LEAVE_GROUP`、`IN6_IS_ADDR_*` の macro、`struct in6_pktinfo` と `IPV6_RECVPKTINFO`（DHCPv6 が受けた interface を知る） |
| `netif.h` の IPv6 の address | `SIOCAIFADDR_IN6`・`SIOCDIFADDR_IN6`（`struct in6_aliasreq`: 名前・`sockaddr_in6`・prefix 長・flag・valid と preferred の寿命）、`SIOCGIFADDRS_IN6`（interface の address の一覧と状態）、`SIOCSIFINET6`（interface の IPv6 の有効・無効） |
| `route.h` の IPv6 の route | `AF_INET6` の socket の `SIOCADDRT`・`SIOCDELRT`・`SIOCGRTENTRY` は `struct in6_rtentry`（宛先・prefix 長・gateway・interface・flag・metric・寿命）を取る（`struct sockaddr` に `sockaddr_in6` が入らないため） |
| route socket の事象 | `RTM_ROUTERADV`（interface・送り元・RA の本体）、`RTM_ADDRINFO`（interface・address・prefix 長・遷移: preferred・duplicate・deprecated・expired）、`RTM_NEIGHBOR`（router の到達性の失敗） |
| `socket()` | `AF_INET6` の `SOCK_STREAM`・`SOCK_DGRAM`、`SOCK_DGRAM` + `IPPROTO_ICMPV6`（今の IPv4 の ICMP の socket と同じく echo だけ、ping 用） |

## 4. networkd（SLAAC と DNS）

- link-local: interface が上がり IPv6 が有効なら、networkd が `fe80::/64` に RFC 7217 の IID を入れる（MAC を出さない、§9 H4）。kernel が DAD して RS を送る。
- 安定な address（RFC 7217）: IID = SHA-256（prefix、interface の名前、network の ID（Wi-Fi は SSID、有線は空）、DAD の回数、秘密）の下位 64 bit。秘密は初回に作る
  32 byte の乱数（`/var/db/networkd/ipv6-secret`、0600）。DAD の失敗は回数を増やして作り直す（最大 3 回）。`userland/base/common/sha256.h` を使う。
- 一時的な address（RFC 8981）: 乱数の IID、preferred ≤ 1 日（−DESYNC）、valid ≤ 2 日、切れる前（REGEN_ADVANCE）に次を作る。既定で有効。
- RA の処理: PIO の A flag で SLAAC、L flag で on-link の route、router lifetime で既定の route（router preference、RFC 4191 の Route Information は v1 で受けるだけ）、
  RDNSS・DNSSL で resolver、M flag で `dhcpc -6`（stateful）、O flag で RDNSS が無ければ `dhcpc -6 -i`（stateless）。寿命が切れた物は kernel が消し、networkd は通知で表を直す。
- 既定の route: IPv4 と同じ有線の優先（ws005-p019 の network の優先）。優先の interface の router だけを入れる。carrier が落ちたら、その interface の SLAAC・DHCPv6 の address と route を消す。
- resolver: 優先の interface の DHCPv4 の DNS、RDNSS、DHCPv6 の DNS の順に、最大 3 つを `resolv.conf` に（§9 H5）。`net.conf` の static・merge の DNS の扱いは今と同じ。

## 5. `dhcpc -6`

- `dhcpc -6 [-i] IF`: `-i` は Information-Request（DNS・domain）。無ければ Solicit・Advertise・Request・Reply で IA_NA を取り、address（寿命つき）を入れ、DNS を書く。
  Rapid Commit は使わない。DUID は DUID-UUID（RFC 6355、初回に作って `/var/db/dhcpc/duid` に保つ、MAC を出さない、§9 H7）。
- 更新: IPv4 の `dhcpc` と同じく 1 回で終わる。networkd が T1 で `dhcpc -6` を走らせ直す（Renew）。IPv4 の更新が無いのは今の制限（WS001 の台帳）で、この WS では IPv6 だけを扱う。
- UDP socket（`AF_INET6`、port 546、`ff02::1:2` へ、`SO_BINDTODEVICE` と `sin6_scope_id`）。packet socket は使わない。

## 6. `net.conf`

```yaml
interfaces:
  ue0:
    type: ethernet
    enabled: true
    ipv4:
      enabled: true        # 新しい。無ければ true（今の file はそのまま）
      dhcp: true
    ipv6:
      enabled: true        # 無ければ true（loopback は ::1）
      autoconf: true       # RA の SLAAC
      dhcp: auto           # auto（RA の M/O に従う）| stateless | stateful | false
      stable-address: true # RFC 7217。false なら EUI-64（MAC を含む）
      temporary: true      # RFC 8981
      addresses:
        - address: 2001:db8::10
          prefix-length: 64
routes:
  - destination: ::/0
    gateway: fe80::1
    interface: ue0         # 新しい。link-local の gateway に要る
dns:
  mode: dhcp               # 自動（DHCPv4・RDNSS・DHCPv6）
  servers: [2001:db8::53]  # IPv6 の literal を受ける
```

- `version: 1` のまま、無い key は既定。`ipv6:` が無い interface は IPv6 が有効（§9 H3）。`net` の保存（`netconf_write`）は既定と違う key だけを書く。
- Wi-Fi の interface は `net.conf` の外なので、v1 は既定（有効）で動き、network ごとの `ipv6:` は wifi-store に後で足す（§9 H8）。
- `netconf.h` の `NETCONF_IPV4_MAX`（15）の文字列の欄は IPv6 の literal（最大 39、scope つきで 39+16）に広げる。

## 7. libc と道具

- `inet_pton`・`inet_ntop`（RFC 5952）、`getaddrinfo`: A と AAAA を問い、RFC 6724 の宛先の並べ方（global の IPv6 の source が無ければ AAAA を後ろに。IPv6 の address は有るが外に出られない network で
  application が止まらないように）、`AI_ADDRCONFIG`・`AI_V4MAPPED`。`getnameinfo` の `%scope`。resolver の `nameserver` に IPv6 と `fe80::…%ue0`。
- `ifconfig`（`inet6` の行、`inet6 ADDR/LEN` の追加・`-inet6` の削除）、`route`（`-6`、`::/0`）、`ping`（IPv6 の literal と名前、`-6`・`-4`）、`host`・`nslookup`（AAAA）、`fetch`（IPv6 の URL）。
  `ndp` の command（近隣の一覧）は Future Work。

## 8. 段（Phase の案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | kernel の核: UAPI の差分（H1 の後）、`ipv6.c`・ICMPv6（echo・error）・NDP・DAD・MLDv2・address と route の表と ioctl、RS・RA と route socket の事象、loopback `::1` | H1〜H4・H6 |
| p003 | transport: address を 16 byte に（IPv4 の振る舞いは変えない、T1 の IPv4 の回帰）、`AF_INET6` の UDP・TCP・ICMPv6 の socket、`IPV6_V6ONLY`、PMTU、source の選択 | p002、H2 |
| p004 | libc: `inet_pton`・`inet_ntop`・`getaddrinfo`・`getnameinfo`・resolver（host 試験: RFC 5952 の表記、RFC 6724 の並べ方） | p003（host 試験は先に書ける） |
| p005 | 道具と `net.conf`: `netconf` の `ipv6:` の節（parse・validate・write の host 試験）、`net` の静的な IPv6、`ifconfig`・`route`・`ping`・`host`・`nslookup` | p003、p004、H3 |
| p006 | networkd の SLAAC: link-local・RFC 7217・RFC 8981、RA の処理、既定の route、RDNSS・DNSSL、DAD の失敗、carrier（host 試験: RA の解析、IID の決定性） | p002、p005、H4・H5 |
| p007 | `dhcpc -6`: stateless・stateful、DUID、networkd からの起動と Renew（host 試験: message の codec） | p006、H7 |
| p008 | T1: QEMU の slirp（link-local・SLAAC の `fec0::/64`・RDNSS・information-request・`ping fec0::2`・host への TCP）と、host の tap と network namespace の dnsmasq（M=1 の stateful・O=1・RDNSS 無し）を 1 つの QEMU で。5330 の UAT（家の router の IPv6、Wi-Fi） | p007 |
| p009 | 全文規約の見直し | p008 |

- Settings の Network の頁の IPv6 の表示と設定は WS089（P2）と調整し、p006 の後に別の Phase か WS で。

## 9. 人間の判断が要る点

| ID | 問い | 案 |
| --- | --- | --- |
| H1 | UAPI（§3.6） | 表の形で承認を得て、正確な差分は p002 の最初に出して適用前にもう一度確かめる |
| H2 | `IPV6_V6ONLY` の既定 | 0（Linux・RFC 3493 の既定。`[::]` で listen する移植の application が IPv4 も受ける。§3.1 の案 A で自然に書ける）。BSD の 1 は採らない |
| H3 | 既存の `net.conf` での IPv6 の既定 | 有効（macOS・Windows・Linux と同じ）。`ipv6: enabled: false` で止める |
| H4 | link-local の IID | RFC 7217（RFC 8064 の推奨、MAC を出さない） |
| H5 | dual stack の DNS の順 | DHCPv4 の DNS、RDNSS、DHCPv6 の DNS の順に最大 3 つ |
| H6 | fragment | v1 は再構成しない（IPv4 と同じ）。Packet Too Big は扱う。必要になったら別の段 |
| H7 | DHCPv6 の DUID | DUID-UUID（乱数、保存して安定、MAC を出さない） |
| H8 | Wi-Fi の network ごとの IPv6 の設定 | v1 は既定（有効）だけ。wifi-store の `ipv6:` は後 |

## 10. 試験の方法

- host: RFC 5952 の表記・RFC 6724 の並べ方（libc）、`netconf` の round trip、RA の解析と SLAAC の決定（networkd の部分を host で組む）、DHCPv6 の codec、
  kernel の NDP の状態遷移（`nd6.c` を host で組む、近隣の cache・DAD の timer）。
- QEMU（T1、p008 でまとめて 1 回）: 試験の image は `plan/ws130/tests/` の config.mk と個別の file の複写。slirp の IPv6 と、tap と netns の dnsmasq（`sudo apt install dnsmasq`、host の操作）。
  判定は guest の `ifconfig`・`route`・`ping`・`host` の出力（SSH）。console・serial の log は使わない。
- 実機（UAT）: 5330 の Wi-Fi と USB の有線で、家の router の SLAAC・DNS、一時的な address の外への接続、IPv6 だけの site への接続。

## 結果

（設計の第 1 版。判断 H1〜H8 待ち）
