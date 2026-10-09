<!-- awesome-plan project=zedbsd record=ws177-p046 -->

# ws177-p046: dhcpc の DHCPv6 の準正常系・異常系と DNSSL の不正な label（案 R）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-09 Q1 判定: T1-500 で ipv6-r-dnsmasq・p008 とも PASS（`one IPv6 default route` ok）。networkd の直しは P1 cd50a24ea）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q909（P1、承認は p044 と同じ）
Origin: [backlog-p1](../backlog-p1.md) 25・26・28 行（WS130 ws130-p007）、[案](../phasing-20261008.md) の R。

## 範囲

- 25（`userland/base/dhcpc/inet6.c`）: T1 の Renew に答えが無ければ T2 で Rebind（Server Identifier なし、全ての server へ）、interface を
  下ろす時に Release、DAD の失敗で Decline。Reconfigure は扱わない理由を記録するか、受けて Renew する。
- 26: Renew で別の address になった時・Solicit からやり直した時に、記録の前の address を SIOCDIFADDR_IN6 で消す（`inet6_finish`）。
- 28（`networkd/slaac.c`・`net/dhcp6.c`）: DNSSL・DOMAIN_LIST の名前の途中で不正な label に当たったら、その名前を丸ごと捨てる（前半を残さない）。

## 実装（2026-10-08 夜 P1）

- 25（`userland/base/dhcpc/inet6.c`、`net/dhcp6.c`）: DHCPv6 の型に Rebind（6）・Release（8）・Decline（9）。Release・Decline は server の
  identifier と address が要り、ORO を付けない（RFC 8415 21.7）。記録（`/var/db/dhcpc/IF.dhcp6`）に `obtained`（取った時の wall clock の秒）・
  `t2`・`valid` を足し（古い記録は 0・無限として読む）、`dhcp6_lease_next` が経過で Renew（T2 の前、記録の server へ）・Rebind（T2 から
  有効期間の終わりまで、server を名指さない）・Solicit（その後）を選ぶ（`inet6_rebound`、走る時間の半分）。答えが無ければ今と同じく
  Solicit へ。T1・T2 は `dhcp6_lease_times`（server の物、無ければ preferred の 1/2・4/5、T2 は T1 より前にしない）。
  `dhcpc -6 -r IF`（Release）: 記録が無ければ何もしない。記録の server へ Release、答えの有無にかかわらず address を外し記録を消す。networkd が
  interface の DOWN と IPv6 の off の前に `dhcpc -6 -r -t 2 IF` を走らせ（`release_dhcp6`、結果は要求の結果にしない）、DHCPv6 の予定を忘れる
  （`networkd_ipv6_forget`）。`dhcpc -6 -D IF`（Decline）: 記録の address を Decline して外し、記録を消してから Solicit。networkd は
  RTM_ADDRINFO の DUPLICATE が DHCPv6 の address（IN6_IFF_DHCP）なら、stateful の entry に `decline` を立てて `dhcpc -6 -D` を走らせる。
  Reconfigure（18.2.11）は扱わない: dhcpc は 1 回走るだけで実行の間に待ち受けず、Reconfigure は RFC 8415 の認証の鍵が要る（dhcpc は認証を
  しない）。networkd が T1 に走らせ直す（inet6.c の冒頭に記録）。
- 26（`inet6_finish`）: 記録の前の address と違う address を得たら（Renew・Rebind・Solicit のどれでも）前の物を SIOCDIFADDR_IN6 で外す。
- 28（`networkd/slaac.c` の `slaac_dnssl`、`net/dhcp6.c` の `dhcp6_domains`）: 名前の途中で host 名でない label、または入りきらない label に
  当たったら、その名前を丸ごと捨てて次の名前を読む（option の端を越える label は、その名前と後ろを捨てて終わる）。前の option の名前の後に
  続ける時に空白で区切る（以前は 2 つ目の DNSSL の最初の名前が前の名前に `.` で繋がった）。

## 確認（2026-10-08 夜 P1）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws177/tests/host-ipv6-r.sh`（dhcp6・names の群を足した、plain と ASan+UBSan） | 67 checks PASS |
| `sh plan/ws130/tests/host-dhcp6.sh build/p1-ws130-host`・`host-slaac.sh`（既存） | PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/dhcpc build/p1-uat/bin/networkd build/p1-uat/bin/net` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（変えた関数） | 指摘 0（inet6.c・dhcp6.c の触っていない ws130 の関数の既存の指摘は残る） |

未実施（T1）: `plan/ws177/tests/ipv6-r-dnsmasq.sh`（新、ws130 の ipv6-p008-dnsmasq.sh の segment）の Rebind・前の address・Decline・Release
（p045 の確認と一緒）。残る小さな窓: networkd の非同期の `dhcpc -6` が走っている間に DOWN の `dhcpc -6 -r` が同じ port を開けないと
Release は送られない（address と記録はその dhcpc の終わりの後も残る）。

## 確認の予定（元の案）

- host: `host-ipv6-r.sh` に DNSSL・DOMAIN_LIST の壊れた名前、dhcpc の状態機械の偽の server（既存の `plan/ws130/tests/host-dhcp6.c` の形）。
- build warning 0、style-check。QEMU（T1）: ws130 の `ipv6-p007.sh`（dnsmasq）に Rebind（server を止める）・Release の確認。
