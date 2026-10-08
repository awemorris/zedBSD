<!-- awesome-plan project=zedbsd record=ws177-p046 -->

# ws177-p046: dhcpc の DHCPv6 の準正常系・異常系と DNSSL の不正な label（案 R）

Parent: [WS177](../ws.md)
Status: planned（2026-10-08 夜 P1 q909 で立てた。実装は未着手）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q909（P1、承認は p044 と同じ）
Origin: [backlog-p1](../backlog-p1.md) 25・26・28 行（WS130 ws130-p007）、[案](../phasing-20261008.md) の R。

## 範囲

- 25（`userland/base/dhcpc/inet6.c`）: T1 の Renew に答えが無ければ T2 で Rebind（Server Identifier なし、全ての server へ）、interface を
  下ろす時に Release、DAD の失敗で Decline。Reconfigure は扱わない理由を記録するか、受けて Renew する。
- 26: Renew で別の address になった時・Solicit からやり直した時に、記録の前の address を SIOCDIFADDR_IN6 で消す（`inet6_finish`）。
- 28（`networkd/slaac.c`・`net/dhcp6.c`）: DNSSL・DOMAIN_LIST の名前の途中で不正な label に当たったら、その名前を丸ごと捨てる（前半を残さない）。

## 確認の予定

- host: `host-ipv6-r.sh` に DNSSL・DOMAIN_LIST の壊れた名前、dhcpc の状態機械の偽の server（既存の `plan/ws130/tests/host-dhcp6.c` の形）。
- build warning 0、style-check。QEMU（T1）: ws130 の `ipv6-p007.sh`（dnsmasq）に Rebind（server を止める）・Release の確認。
