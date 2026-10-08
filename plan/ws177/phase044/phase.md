<!-- awesome-plan project=zedbsd record=ws177-p044 -->

# ws177-p044: IPv6 の libc と道具（案 R2）

Parent: [WS177](../ws.md)
Status: test-wait 予定（2026-10-08 夜 P1 q909: 実装・host PASS・build warning 0。T1 の依頼を Q1 へ（p045・p046 とまとめて））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q909-i01（P1、承認: Q1 の投入「WS177 案 R・R2: IPv6 の networkd・dhcpc と libc・道具（backlog-p1 14〜28）。p1 18 の IPV6_RECVHOPLIMIT の UAPI 追加は Q1 が可とする（HAL ではない）」）
Origin: [backlog-p1](../backlog-p1.md) 14・15・16・18・19 行（WS130 ws130-p004・p005）、[案](../phasing-20261008.md) の R。

## 範囲と設計

- 14: `getaddrinfo` の `AI_ADDRCONFIG` を interface の address で判定（RFC 3493）。`gai_configured` が SIOCGIFCONF で interface を数え、SIOCGIFADDR（IPv4）と
  SIOCGIFADDRS_IN6（IPv6）を見る。IPv4 は 0.0.0.0 と 127/8 以外、IPv6 は DAD が済んだ（tentative・duplicated でない）::・::1・fe80::/10 以外
  （`resolver_usable4`・`resolver_usable6`、resolver-dns.c、host で試す）。interface が読めない時だけ今の「最初の address への UDP の connect」。
- 15: `getnameinfo` の service を、`NI_NUMERICSERV` で無ければ `/etc/services` の名前（`NI_DGRAM` で udp、他は tcp。`gai_service_name`）、
  `NI_NOFQDN` で PTR の名前を最初の label に（`resolver_short_name`）。
- 16: `resolver_result` に答えた server の family・IPv6 の address・scope（`server_family`・`server6`・`server6_scope`）。`resolver_query_server6`
  が入れる。nslookup が `Server: fe80::1%ue0#53` と出す。
- 19: nslookup の server の引数に IPv6（`%zone` は名前か番号）。`-p` の別の port でも resolv.conf の IPv6 の server を使う。
  `resolver_parse_server6`・`resolver_query_server6` を libc の内部の口（resolver-internal.h）に。
- 18: ping -6 の `hlim=`。kernel: UAPI `IPV6_RECVHOPLIMIT`（37）・`IPV6_HOPLIMIT`（47、FreeBSD の番号）、`recvmsg_args.reserved2` を
  `hop_limit` に改め、出力の `RECVMSG_HOP_LIMIT`（output_flags の bit 31）で中身があることを示す（入力は今と同じく 0 で、古い libc・kernel と
  互換）。`socket_ops.recvfrom_hop`（無い protocol は今の recvfrom）、`packet_buf.hop_limit`・`hop_limit_known`、raw ICMPv6 の socket の
  setsockopt（`IPV6_RECVHOPLIMIT`、他は inet の物、知らない物は今と同じく ENOPROTOOPT）、`icmp6_raw_deliver` が求めた socket の copy に
  IPv6 header の hop limit を付ける。libc の recvmsg が `IPPROTO_IPV6`/`IPV6_HOPLIMIT` の control message にする（場所が無ければ MSG_CTRUNC）。
  ping6 は recvmsg で受け、`64 bytes from …: icmp_seq=1 hlim=64 time=…`。

## 確認（2026-10-08 夜、P1）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-ipv6-r.sh`（新、plain と ASan+UBSan） | libc の群 11 checks PASS |
| `sh plan/ws130/tests/host-libc6.sh`（既存） | PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/vmunix build/p1-uat/bin/ping build/p1-uat/bin/nslookup build/p1-uat/dynamic/libc.so` | rc 0、warning 0 |
| style-check（変えた行） | 指摘 0 |

未実施（T1）: `ping -6 -c 2 ::1` の `hlim=`、`nslookup NAME <IPv6 の server>` の Server の行、`getent`/`getaddrinfo` の AI_ADDRCONFIG（IPv6 の
address の無い image で AAAA を返さない）。
