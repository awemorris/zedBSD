<!-- awesome-plan project=zedbsd record=ws315 -->

# WS315 Realtek の USB の 2.5G・5G の NIC を、Realtek の独自の方式（vendor mode）の driver で速くする

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG003。
- Status: completed（2026-10-30 Q1 が受け入れを確かめた。全 Phase cleared: p001〜p005 は P3 q1694-i01、p006 は P3 q1696-i01・q1702-i01）
- 目標: 市場の USB の 2.5GbE・5GbE の NIC のほとんどが使う Realtek の chip（RTL8156・RTL8157 の類）を、汎用の CDC NCM ではなく
  Realtek の独自の方式で動かす driver を作り、Linux（r8152）と同じ程度の速さにする。
- 設計: [design.md](design.md)（出典と license の境界、chip の見分け、vendor の configuration への切り替えと NCM との選び分け、転送の形、PHY・link、§12 review の反映）。

## ユーザーの決定（2026-10-30、原文）

「xHCIの改修は行いましょう。たとえ今回効果がなくても、価値がある実装だと考えます。
Realtek方式については、NCMでどこまで速度が上がるか観察してから、ぜひ実装しましょう。今のところUSB 2.5G NICはRealtekのチップのものしか持っていません。現在、市場にある2.5G USB LAN製品は、ほぼ100%がRealtekのチップのものだそうです。なので、このドライバを実装する価値が非常に大きいです。
5GbEのUSB NICについても、ほとんどがRealtekだそうです。すでに市場から消え始めていますが、Marvell（Aquantia）のチップもありました。AQCはUSB4で10GbEのものを1つ持っていますが、2.5/5GbEは入手が難しいですね。」

（2026-10-30 追記）「5GbEのNICは持っていません。今後購入してテストしたいです。資料だけで実装しておいてほしいです。」

## 結果

- driver `usb-rtl815x`（`src/drivers/usb/usb-rtl815x.c`、frame の純粋な部分 `usb-rtl815x-frame.c`・`include/drivers/usb/usb-rtl815x.h`）: RTL8156・8156B を vendor mode
  （config 1）で動かす。register の読み書き、初期化・up・down、RX の集約と TX の詰め（記述子 v1）、checksum の offload、link の worker、MTU 9000、pure ACK を data の前に出す TX の queue。
  選び分けは match の優先 120、`CONFIG_DRIVER_USB_RTL815X`、boot の token `usb.rtl815x=off`（NCM に戻す）。出典は FreeBSD・OpenBSD の if_ure と Realtek の公開の値、Linux の r8152 は読むだけ（独立実装）。
- RTL8157（5G）: 記述子 v2、version 16 の初期化、PHY の表、5000 の広告を資料だけで実装した（host 試験・build のみ、ユーザーの決定「資料だけで実装しておいてほしいです。」）。
- TCP の受けの buffer の自動調整（F-WS315-rcvbuf の解決、p006）: 64 bit の機械で受けの buffer を 128 KB から 1 往復の量の 1.25 倍へ自動で広げる（接続ごと 1 MB、全体で pool の 1/4、
  SO_RCVBUF で固定）。pool 4096、送りの ring 768・送りの buffer 1152 KB。32 bit の板は変えない。
- 速さ（5330 の RTL8156B 0x7410、相手 centris、30 s、Mbit/s）:

| | send | recv | 同時 send | 同時 recv |
| --- | --- | --- | --- | --- |
| zedBSD vendor mode（p006 の最終） | 2369.9 | 2372.9 | 2268.3 | 2359.2 |
| zedBSD CDC NCM（同じ adapter、`usb.rtl815x=off`、p003 の k2・k3） | 2062.8・1971.0 | 1473.7・1479.9 | 未実施 | 未実施 |
| Linux r8152 | 2353.4 | 2353.6 | 2345.7（負荷の下 2253.7） | 2340.9（負荷の下 2310.8） |

- 非退行: 5330 の ping 1472・MTU 9000/1500 の変更・down/up・内蔵の USB（uvc・ccid・AX211 の bt）の attach、10GbE（RTL8127）の send・recv・同時、QEMU の qemu-tcp、
  tcp-model、21 config の vmunix の build warning 0。

## 制限・移管

- 抜き差し・相手の port の上げ下げ: 5330 は遠隔で抜けず、相手は switch の先 → ユーザーの拾える試験の一覧（master.md）。
- RTL8157（5G）の実機: 手元に無い → ユーザーの購入の後に試す（ユーザーの拾える試験）。RTL8156（A 版）・0x7400・0x7420 の実機も未実施（手元は 0x7410 だけ）。
- jumbo の相手（MTU 9000 の ping の相手）が無い: 9000 への変更と戻しだけ確かめた。
- 5330 の内蔵 camera の stream（camcap frames=0）は WS282 の範囲（Q1 の判断）。
- ue0 の TX dropped は driver の TX の queue（128）の満杯で返した手渡し（ifi_oqdrops、TCP が渡し直す、wire の損失ではない）。
- 範囲の外（記録だけ）: Marvell（Aquantia）AQC の USB の NIC。
- 今後も使う試験: [plan/tools/rtl815x/](../tools/rtl815x/)（5330-rtl.sh（出力 build/rtl815x-hw）・config-5330-rtl.mk・5330-linux-both.sh・rtl815x-frame-test.c・run-rtl815x-frame-test.sh、[plan/tools.md](../tools.md)）。

## Phase（完了）

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws315-p001 | 設計（design.md） | cleared 2026-10-30 04:04 |
| ws315-p002 | 実装: RTL8156・8156B の vendor mode、host 試験、全 platform の build | cleared 2026-10-30 04:16（16122571a） |
| ws315-p003 | 5330 の実機と調整（pure ACK を前に） | cleared 2026-10-30 06:15 |
| ws315-p004 | RTL8157（資料だけ） | cleared 2026-10-30 05:43 |
| ws315-p005 | 規約の全文の見直し | cleared 2026-10-30 05:53 |
| ws315-p006 | TCP の受けの buffer の自動調整（F-WS315-rcvbuf）、送りの ring 768 | cleared 2026-10-30 10:45（8b9523b20・4575db11d・f49185c5b） |
