<!-- awesome-plan project=zedbsd record=ws315 -->

# WS315 Realtek の USB の 2.5G・5G の NIC を、Realtek の独自の方式（vendor mode）の driver で速くする

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG003。
- Status: incomplete（2026-10-30 10:45 P3: p006 も cleared、全 Phase が cleared、WS の受け入れと completed の判定は Q1 待ち。06:15 P3: p001〜p005 cleared。2026-10-30 Q1 が作成、P3 q1694-i01 が p001 に着手）
- 目標: 市場の USB の 2.5GbE・5GbE の NIC のほとんどが使う Realtek の chip（RTL8156・RTL8157 の類）を、汎用の CDC NCM ではなく
  Realtek の独自の方式で動かす driver を作り、Linux（r8152）と同じ程度の速さにする。

## ユーザーの決定（2026-10-30、原文）

「xHCIの改修は行いましょう。たとえ今回効果がなくても、価値がある実装だと考えます。
Realtek方式については、NCMでどこまで速度が上がるか観察してから、ぜひ実装しましょう。今のところUSB 2.5G NICはRealtekのチップのものしか持っていません。現在、市場にある2.5G USB LAN製品は、ほぼ100%がRealtekのチップのものだそうです。なので、このドライバを実装する価値が非常に大きいです。
5GbEのUSB NICについても、ほとんどがRealtekだそうです。すでに市場から消え始めていますが、Marvell（Aquantia）のチップもありました。AQCはUSB4で10GbEのものを1つ持っていますが、2.5/5GbEは入手が難しいですね。」

（2026-10-30 追記）「5GbEのNICは持っていません。今後購入してテストしたいです。資料だけで実装しておいてほしいです。」

## 前提（Q1 の理解、設計で置き換える）

- 試験の機械: 5330 と RTL8156（2.5G）。5G の Realtek の NIC（RTL8157 の類）は無い: 資料だけで実装し、実機の試験は購入の後（ユーザー）。
- 比べる値: 5330 の Debian の r8152 で 2353 Mbit/s（向きは未記録、測り直す）。WS221 p013・p014 の NCM の値。
- 出典の候補: Linux の r8152 は GPL なので読むだけで写さない（独立実装）。FreeBSD・OpenBSD の `if_ure`（BSD）が RTL8152/8153/8156 を扱う。
  Realtek の公開の資料。license の境界は master-design-policy.md と Guardrail に従う。
- 範囲の外（記録だけ）: Marvell（Aquantia）AQC の USB の NIC。手元の AQC は USB4 の 10GbE（Thunderbolt・USB4 の PCIe tunnel の上の AQC107 の類か、未確認）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws315-p001](phase001/phase.md) | 設計: Realtek の vendor mode（device の切り替え、RX の集約・TX の詰め・checksum と TSO の offload、PHY・link）、NCM との切り替え、出典と license（[design.md](design.md)） | cleared（2026-10-30 04:04 P3 q1694-i01、review を design.md §12 に反映） | WS221 p014 |
| [ws315-p002](phase002/phase.md) | 実装: register の読み書き、RTL8156・8156B の初期化・up・down、RX・TX の block（記述子 v1）、checksum、link の worker、選び分け（match 120・CONFIG・boot の token）、host 試験、全 platform の build | cleared（2026-10-30 04:16 P3、16122571a） | p001 |
| [ws315-p003](phase003/phase.md) | 5330 の実機: send・recv 30 s、NCM・Linux との比べ、USB の他の device の非退行、調整（design.md §5.4・§8） | cleared（2026-10-30 06:15 P3: image の試験 img1 と調整 k6。send 2372.0・recv 2338.3、同時 send 2346.5・recv 1442.9（調整の前 1037.1、Linux の同時 2345.7・2340.9、残りの差は TCP の受けの window 128 KB と見る）、ping・MTU・down/up・他の USB の device PASS。camcap は WS282 の範囲。抜き差しは未実施） | p002 |
| [ws315-p004](phase004/phase.md) | RTL8157（資料だけ）: 記述子 v2、version 16 の初期化、5000 の広告、host 試験、build。実機はユーザーの購入の後 | cleared（2026-10-30 05:43 P3、host 試験 PASS・amd64・pcat の build warning 0。実機は未実施） | p002 |
| [ws315-p005](phase005/phase.md) | 規約の全文の見直し | cleared（2026-10-30 05:53 P3、conformance-reviewer。host 試験 PASS、build warning 0） | p002〜p004 |
| [ws315-p006](phase006/phase.md) | TCP の受けの buffer の自動調整（F-WS315-rcvbuf、2026-10-30 ユーザー「F-WS315-rcvbufを解決してほしいです。WS315のphaseにして、今取り組んでください。」） | cleared（2026-10-30 10:45 P3 q1702-i01: 受けの自動（倍率 1.25 倍）と 64 bit の送りの ring 768。5330 の同時 send 2268.3・recv 2359.2（Linux 2254〜2346・2311〜2341）、片方向・10GbE は落ちない） | p003 |

## Resume point

- 2026-10-30 Q1: 作成。WS221 p014（xHCI）と NCM の観察の後に担当を割り当てる。
- 2026-10-30 P3（q1694-i01）: p001 cleared（design.md、review の反映 §12）。p002 の実装の初版 5534277a3、5330 の試験（plan/ws315/tests/5330-rtl.sh）へ。
- 2026-10-30 P3 checkpoint（363ab924b、context の残りが少なく返却）: p002 cleared。p003 は 5330 の k4 で vendor mode send 2372.2・recv 2319.1（Linux 2353.4・2353.6）、
  未実施の項目（camcap・送受信の同時・port の上げ下げ・抜き差し・jumbo の相手・他の版の RTL8156）が残る。次: p003 の残り（image の試験で camcap と送受信の同時）、
  p004 RTL8157（Linux の r8156_init の version 16 の枝・r8157_hw_phy_cfg・ADV_CMD の BMU/IP の間接の読み書き・EARLY_SIZE /16・IDLE_SPDWN を飛ばす、design.md §11・§12 m2）、
  p005 規約の全文の見直し。merge は Q1 の判断（M4: 既定で vendor mode に切り替わる。5330 の k4 で動いた）。
- 2026-10-30 P3（再開、q1694-i01）: p003 の残り: 5330-rtl.sh に送受信の同時（2 本目の peer、port 5312）、config-5330-rtl.mk に camcap（c3957e2bf）、image build/ws315-img/hdd-image.img を作った。
  5330 の Debian への SSH（uname の確かめ）が auto mode の security の判定（Production Reads）で止まり、Q1 経由でユーザーの判断待ち（5330 には一切触れない）。
  相手の port の上げ下げは centris が switch の先なので centris の down では 5330 の link が落ちない: 実施せず、switch 側の抜き差しとしてユーザーの拾える試験へ（Q1）。
  p004 cleared（RTL8157 の初期化・PHY の表・up・MTU の値、host 試験と build）。次: p003 の image の試験（承認の後、`CAMERA=1 MODE=--nvme-image sh plan/ws315/tests/5330-rtl.sh build/ws315-img/hdd-image.img img1 30`、
  image は p004 の前の tree なので p004 を含めるなら作り直す）、p005 規約の全文の見直し。
- 2026-10-30 05:53 P3: p005 cleared（書き方の見直し、RTL8157 の register の名前を独自に、PHY の表の出典の確かめを p004 に）。残り: p003 の image の試験（5330 の SSH の判断の後）。
  p004・p005 の tree を 5330 で試すなら image を作り直す（`make ZEDBSD_CONFIG=plan/ws315/tests/config-5330-rtl.mk BUILD=build/ws315-img disk-image`）。merge は Q1 の判断（RTL8156B の 5330 の回帰の後）。
- 2026-10-30 06:15 P3（q1694-i01、5330 の SSH の承認の後）: p003 cleared。image（main を merge した f03d745e3 の tree）の img1 で camcap・同時の送受信・ping・MTU・down/up。
  同時の受けが 1037 で、TX queue で純粋な ACK を data の前に出す調整（k6）で 1443（Linux 2341）。残りの差は TCP の受けの window（128 KB、stack の側、範囲の外）と見る。
  camcap の frames=0 は WS282 の範囲（Q1 の判断）。全 Phase が cleared: WS の受け入れ（Linux と同じ程度の速さ: send・recv の片方向は満たす、同時は上の差）と completed の判定は Q1。
  5330 は Debian に戻った（NVMe の zedBSD は img1 の userland と k6 の kernel）。
- 2026-10-30 09:16 P3（q1696-i01、ラップアップ）: p006 を追加（ユーザー「F-WS315-rcvbufを解決してほしいです。WS315のphaseにして、今取り組んでください。」）。受けの buffer の自動調整を実装（8b9523b20）、
  同時 recv は Linux 並みになったが同時 send が落ちた。再開は [phase006](phase006/phase.md) の「引き継ぎ」（次: 1.5 倍の g12 を 5330 で、倍率の決め、10GbE）。5330 は Debian、NVMe の zedBSD の kernel は g10。
- 2026-10-30 10:45 P3（q1702-i01）: p006 cleared。同時の send の退行は、受けの窓が送りの in flight（ring 256）を超えると両方向の比で送りが落ちる事と、2 倍の倍率が switch に作る queue: 64 bit の送りの ring 768・TCP_SEND_BUFFER 1152 KB（Q1 が採った）と倍率 1.25 倍で解いた（4575db11d・f49185c5b）。5330 は Debian（NVMe の zedBSD の kernel は f1）、10.0.10.3 は返した。
