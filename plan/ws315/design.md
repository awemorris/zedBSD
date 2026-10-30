<!-- awesome-plan project=zedbsd record=ws315-design -->

# WS315 設計: Realtek の USB の 2.5G・5G の NIC の vendor mode の driver

- 親: [ws.md](ws.md)、Phase: [p001](phase001/phase.md)。作成 2026-10-30 P3（q1694-i01）。
- 基準（[WS221 p014](../ws221/phase014/phase.md)）: 5330 の RTL8156 で zedBSD の CDC NCM は send 2097・recv 1449 Mbit/s、Debian の r8152 は send 2353.4・recv 2353.6
  （同じ tcp-perf、相手 10.0.10.2）。受けは xHCI の要求の数（4）が天井でなかった。

## 1. 出典と license の境界

| 出典 | license | 使い方 |
| --- | --- | --- |
| FreeBSD 15.0 `sys/dev/usb/net/if_ure.c`・`if_urereg.h`（local: `/home/awe/freebsd/port-analysis/src-15.0/`） | BSD-2-Clause | 主の出典。register の offset・bit・値と初期化の手順を、block ごとに「Value(s) referenced:」「Procedure referenced:」で示して使う。code・名前は写さず書き直す |
| Linux の `drivers/net/usb/r8152.c`（upstream 7.3 の開発版、local: `/home/awe/zedBSD-worktrees/p7-ws313/build/ref/linux-upstream/`） | GPL-2.0 | **読むだけ**。値（register・bit・定数・version の表・descriptor の配置）は guardrail「MIT・GPL の出典のデータの引用」により block ごとに出典を示してデータとして使う。手順は「Procedure referenced:」で示して参考にする。code・comment・構造・symbol の名前は写さない |
| Realtek の公開の資料 | — | RTL8156・RTL8157 の register の公開の datasheet は見つからない（Realtek が公開するのは Linux 用の r8152 の source で GPL）。製品の型番・速度の事実だけ |

- zedBSD の code は Zlib。file の先頭に出典を書かない（guardrail の「出典は該当箇所に block 単位で」）。
- RTL8157 の手順は FreeBSD に無い（FreeBSD 15.0 の if_ure は RTL8152/8153/8153B/8156/8156B だけ）ので、Linux の値と手順を上の規則で使う。

## 2. 対象の chip と見分け

- 見分けは register `PLA_TCR0`（0xe610）の上の 16 bit（= `PLA_TCR1` 0xe612）を mask 0x7cf0 で読んだ値（FreeBSD・Linux とも同じ）:

| 値 | chip | この driver | 記述子 | RX の block |
| --- | --- | --- | --- | --- |
| 0x7020・0x7030 | RTL8156（A） | 対象（5330 の物はどちらかを実機で読む） | v1（RX 24 byte・揃え 8、TX 8 byte・揃え 4） | 48 KB |
| 0x7400・0x7410・0x7420 | RTL8156B・RTL8156BG の類 | 対象（実機なし、RTL8156 と同じ道の版の違い） | v1 | 48 KB |
| 0x1030 | RTL8157（5G） | 対象、資料だけ（p004） | v2（RX 16 byte・揃え 16、TX 16 byte・揃え 16） | 32 KB |
| 0x2020 | RTL8159（10G） | 範囲の外（attach で拒否、log） | v2 | — |
| 0x4c00〜0x6010・0x6400・0x4800 | RTL8152・RTL8153 の類（100M・1G） | 範囲の外（拒否、NCM・ECM のまま） | — | — |

- USB の id: Realtek 0x0bda の 0x8156・0x8157 を ID の表に置く（Linux の表の事実）。他社の VID の RTL8156 の製品（Lenovo・TP-Link など）は手元で確かめられないので
  初めは入れない（ID を足すのは一行、Future Work）。

## 3. vendor の configuration への切り替えと NCM との選び分け

- RTL8156 は configuration を 3 つ持つ（5330 の物の Debian の `lsusb -v`: 1 = vendor（interface class 0xff/0xff/0、bulk-in・bulk-out・interrupt-in）、
  2 = CDC NCM、3 = CDC ECM。WS221 p002・p003 の記録）。
- zedBSD の USB core は configuration ごとに登録された driver の `match` の点の最大を比べ、最大の configuration を選ぶ（`device_preferred_configuration`、
  `src/drivers/usb/usb.c`。同点は記述子の順）。今は NCM（100）の config 2 が選ばれる。
- 新しい driver `usb-rtl815x` は interface class 0xff・subclass 0xff・protocol 0 と VID・PID で ID を照合し、`match` で **120** を返す（NCM の 100 より上）。
  これで core が config 1 を選ぶ。USB core・NCM の driver は変えない。
  - interface の endpoint が bulk-in 1・bulk-out 2・interrupt-in 3 の形（Linux の `rtl_check_vendor_ok` と同じ確かめ、値として）でなければ 0。
- **選び分けの設定**:
  - build: `CONFIG_DRIVER_USB_RTL815X`（Makefile、既定 y、NCM と同じ platform: amd64・pcat・arm64。CDC NCM を持たない config は n）。n なら今と同じ NCM。
  - 起動: boot の parameter の token `usb.rtl815x=off` があれば `match` は 0 を返し、NCM の config 2 が今までどおり選ばれる
    （`kern_boot_parameters_token_present`、usb-hid・pci-vmd と同じ形）。vendor の driver に不具合があった時の逃げ道。
- attach が chip を拒んだ時（範囲の外の version）: configuration は既に 1 で、NCM は付かない（device は使えない）。
  対処: 範囲の外の chip は ID の表に入れない（0x8156・0x8157 の PID は RTL8156・RTL8157 の物）。それでも拒んだら log に version と「usb.rtl815x=off で NCM」を出す。
  attach の中から configuration を変える（core の binding の中で set_configuration を呼ぶ）のは deadlock の危険があり、しない。

## 4. register の読み書き（control 転送）

- vendor request 0x05（読み: bmRequestType 0xc0、書き: 0x40）、wValue = register の address、wIndex = 種別（PLA 0x0100、USB 0x0000）| byte enable。
  4 byte の境界に揃えて dword で読み、1・2 byte は shift。書きは byte enable（dword 0xff、word 0x33、byte 0x11、6 byte 0x3f）を wIndex に OR して shift した値を書く。
  （FreeBSD `ure_ctl`・`ure_read_*`・`ure_write_*` の手順と値。Linux の `pla_ocp_*`・`usb_ocp_*` も同じ値）
- PHY（OCP）の register: `PLA_OCP_GPHY_BASE`（0xe86c）に address の上 4 bit を書き、`0xb000 | (address & 0xfff)` を読み書き（FreeBSD `ure_ocp_reg_*`）。
  MII の register n は OCP の 0xa400 + 2n。SRAM は OCP 0xa436（address）・0xa438（data）。
- 関数: `rtl_read(adapter, type, reg, width, &value)`・`rtl_write(...)`・`rtl_set_bits`・`rtl_clear_bits`・`rtl_ocp_read`・`rtl_ocp_write`。
  control 転送は thread の文脈だけ（attach・open・close・ioctl・link の worker）。timeout 1000 ms。失敗は errno を返し、初期化の途中の失敗は attach の失敗。

## 5. 初期化（attach）と up（open）・down（close）

RTL8156・RTL8156B は FreeBSD の `ure_attach_post`・`ure_rtl8153b_init`・`ure_rtl8153b_nic_reset`・`ure_init` の手順に従う（値と順は block ごとに出典）。
Linux の `r8156_init`・`rtl8156_up`・`rtl8156_enable` と違う所は、まず FreeBSD に従い、実機で問題が出た時に Linux の値を試す（下の 5.4）。

### 5.1 attach（一度）

1. version を読む（§2）。範囲の外は ENODEV と log。
2. chip の初期化（`ure_rtl8153b_init` の 8156 の枝）: USB 0xd26b bit0 を下げ、0xd32a に 0、0xcfee bit 5 を上げ（8156B は 0xb460 bit 3 も）、ALDPS を切る、U1U2 を切る、
   8156B（0x7410）の flash の読み込みを待つ、`PLA_BOOT_CTRL` の autoload の終わりを待つ（10 ms × 最大 100）、PHY の状態（OCP 0xa420 & 7）が ext-init なら
   OCP 0xa468 の bit 1・3 を下げる（8156B は 0xa466 bit 0 も）、BMCR の power down を外す、PHY が LAN_ON（3）になるのを待つ、U2P3 を切る、MSC timer 0x0fff、
   U1U2 timer 500、power cut を切る、UPS を切る、queue wake を切る、runtime suspend を切る、SuperSpeed なら U1U2 を入れる、8156B の flow control の timer（600 ms）と
   patch、`PLA_EXTRA_STATUS` の link の印、MAC の clock の speed down（`PLA_MAC_PWR_CTRL` 0x0403、CTRL2）、RX の集約を有効（`USB_USB_CTRL` の RX_AGG_DISABLE・RX_ZERO_EN を下げる）、
   8156 は USB 0xd4b4 bit 1、tally の reset。
3. MAC address: `PLA_BACKUP`（0xd000）の 6 byte。全て 0 か multicast の bit なら、kernel の乱数で locally administered の address を作り log に書く。
4. xHCI の queue depth（§8）、buffer と URB を確保、`ue<n>` の net device を作る（NCM と同じ名前の付け方、networkd の設定が同じまま効く）。
5. log 1 行: `usb-rtl815x: ue0 chip=RTL8156 version=0x7030 mac=… usb=super mtu_max=9000 urbs=4/4 rx_block=49152`。

### 5.2 open（up）

1. `ure_rtl8153b_nic_reset` の手順: U1U2・U2P3 を切る、ALDPS を切る、`rxdy_gated` を入れる、teredo を切る、`PLA_RCR` の受けを全て下げる、`PLA_CR` の reset（bit 4、10 ms × 最大 100）、
   BMU の reset（`USB_BMU_RESET` の IN・OUT を下げて上げる）、`NOW_IS_OOB` と `MCU_BORW_EN` を下げる、RX の VLAN の剥がしを切る（PLA 0xc012 の bit 6・7 を下げる: zedBSD の stack は
   VLAN を frame のまま扱う）、RMS = MTU + 22（`PLA_RMS`）、MTPS = jumbo、flow control の閾値（8156: 0xc0a6 = 0x0400・0xc0aa = 0x0800、8156B: 0x0200・0x0400）、RX FIFO の閾値（0xc0a2 の下 12 bit = 8、
   `USB_RX_BUF_TH` = 0x00600400）、TX FIFO の閾値（8156: `PLA_TXFIFO_CTRL` = 0x01000008 の下 16 bit・USB 0xd4b4 bit 1、8156B: 0x0008・0xe61a =（RMS + 0x100）/ 16）、
   `PLA_MAC_PWR_CTRL3` の MCU の speed down を下げる、USB 0xd32a の bit 8・9 を下げる、ALDPS を入れる、U2P3 を入れる、SuperSpeed なら U1U2 を入れる。
2. MAC address を `PLA_IDR`（0xc000）へ（`PLA_CRWECR` を config 0xc0 にして 6 byte、戻す）。
3. RX の集約（§6.1）: `USB_RX_EARLY_TIMEOUT`（0xd42c）= 640 / 8 = 80、`USB_RX_EXTRA_AGGR_TMR`（0xd432）= 15000 / 8 = 1875（15 µs）、`USB_RX_EARLY_SIZE`（0xd42e）=
   （RX block − （RMS + RX 記述子 + 揃え））/ 8、`USB_UPT_RXDMA_OWN` に OWN_UPDATE | OWN_CLEAR。8156B は `USB_FW_TASK` の FC_PATCH_TASK を下げて 1〜2 ms 後に上げる。
4. packet filter の reset（`PLA_FMC` の MCU_EN を下げて上げる）、`PLA_CR` の RE・TE を上げる、`PLA_MISC_1` の RXDY_GATED を下げる。
5. 受けの filter: `PLA_RCR` に APM（自分宛）・AB（broadcast）・AM（multicast）、`PLA_MAR0/4` を全て 1（全 multicast を受ける。NCM の ALL_MULTICAST と同じ扱い、IPv6 の ND に要る）。
   promiscuous は今の stack に口が無いので扱わない。
6. 自動交渉の広告（§7）、interrupt の URB と RX の URB を出す。
7. link は interrupt で知る（§7）。open の時に一度 `PLA_PHYSTATUS` を読み、carrier と速さを決める。

### 5.3 close（down）

- URB を全て cancel して drain（NCM と同じ形）、`PLA_RCR` の受けを下げる、`PLA_CR` の RE・TE を下げる。carrier は NCM と同じく close で消さない（BUG-277 の教訓、
  open の時に PHYSTATUS を読み直す）。

### 5.4 Linux との違い（実機で問題が出た時に試す候補、初めは入れない）

- RTL8156（0x7020/0x7030）の nic reset: Linux は `PLA_CR` の reset でなく TE を下げ・BMU の IN の reset・CDC_ECM_EN の上げ下げ・RE を下げる手順。
- PHY の parameter の patch（Linux `r8156_hw_phy_cfg` の SRAM の書き込み群）と PHY の firmware（`rtl8156a-2.fw` など、Linux は任意で load）。FreeBSD はどちらもしない。
- `rtl8156_enable` の速さごとの設定（2500 で `PLA_MAC_PWR_CTRL4` の IDLE_SPDWN を下げる、ifg）。下げ・上げは FreeBSD の `ure_link_state` にもあり、link の worker で入れる（§7）。

## 6. 転送の形

### 6.1 RX の集約（rx_agg）

- device は複数の frame を 1 つの bulk-in の転送に詰める。block の中: [RX 記述子][frame（FCS 4 byte を含む）][次の記述子の揃えまでの詰め] の繰り返し。
  - v1: 記述子 24 byte（opts1 の下 15 bit が長さ（FCS を含む）、opts2 に checksum の種類、opts3 に checksum の失敗の bit）、次の記述子は block の頭から 8 の倍数。
  - v2（RTL8157）: 記述子 16 byte（opts1 の bit 31:17 が長さ、opts3 に種類と失敗）、揃え 16。
  - 長さが 0x7fff 以上（v1）、ETH_ZLEN（60）未満、block の残りより長い時はその block の残りを捨てる（境界を信じられないため。FreeBSD の注記と同じ判断）、rx_errors を 1。
- block の大きさ: RTL8156 は 48 KB（FreeBSD・Linux とも）、RTL8157 は 32 KB（Linux）。RX の URB は 4 本（§8）、buffer は 5（NCM と同じ spare の形）。
- 早く返す条件（device が block を閉じる条件）: EARLY_SIZE（block に入る残り）と時間（EARLY_TIMEOUT 640 ns、EXTRA_AGGR 15 µs）。値は §5.2。
- frame は packet_buf（`PACKET_BUF_ALLOC_RECEIVE`、jumbo は jumbo の class）に copy して stack へ（NCM と同じ）。zero-copy は packet_buf の形に合わず範囲の外。

### 6.2 TX の詰め

- 1 つの bulk-out の転送に frame を詰める: [TX 記述子][frame][揃え]。
  - v1: 記述子 8 byte。opts1 = 長さ | FS（bit 31）| LS（bit 30）、opts2 = checksum の指示（§6.3）。揃え 4。
  - v2: 記述子 16 byte。opts1・opts2 は v1 と同じ、opts3 = 長さ << 4、opts4 = 署名 0x15 << 27。揃え 16。
- block は 16 KB（Linux の agg_buf_sz、FreeBSD の URE_TX_BUFSZ も 16 KB）。jumbo の 9022 byte も 1 つ入る。
- TX の URB は 4 本。pump 1 本が queue の頭から block に入るだけ詰めて submit（NCM の pump と同じ形、frame の順を保つ）。queue は 64（64 bit の機械）。
- frame の FCS は device が付ける。60 byte に満たない frame の詰めも device がする（Linux も詰めない）。

### 6.3 checksum の offload

- TX: net device の capability `NET_DEVICE_CAP_TX_CSUM_IPV4_TCP` を立てる。packet の `PACKET_BUF_TX_CSUM_TCP` に対し opts2 に TCP_CS（bit 30）・IPV4_CS（bit 29）・
  L4 の offset（frame の頭から。zedBSD の `l3_offset`・`l4_offset` は `packet->storage` からの位置なので、`storage + l3_offset − data` に IPv4 の header の長さを足して求める）<< 17（最大 0x7ff、v2 は 0x3ff）。`PACKET_BUF_TX_CSUM_IPV4` は IPV4_CS。offset が上限を超える frame は software で
  `packet_buf_checksum_finish` してから送る（落とさない）。TCP の checksum の field は pseudo header の和（補数にしない）を持つ約束（packet-buf.h）が、Linux の CHECKSUM_PARTIAL と同じ形。
- RX: v1 は opts2 の IPV4_CS（bit 19）・IPV6_CS（bit 20）・TCP_CS（bit 22）・UDP_CS（bit 23）と opts3 の IPF（bit 23）・UDPF（bit 22）・TCPF（bit 21）、
  v2 は opts3 の IPV4（bit 14）・IPV6（bit 15）・TCP（bit 10）・UDP（bit 11）・IPF（bit 26）・UDPF（bit 25）・TCPF（bit 24）。
  確かめて正しい物に `PACKET_BUF_RX_CSUM_IPV4`・`PACKET_BUF_RX_CSUM_L4` を付ける（stack は付いた物の計算を省く: tcp.c・udp.c・ipv4.c）。

### 6.4 TSO

- device は TSO（GTSENDV4、MSS）を持つが、zedBSD の stack に TSO（大きい segment を driver に渡す口）が無い。範囲の外。send の残りの差の主な候補として記録（Future Work の候補、Q1 へ）。

## 7. PHY・link・速さ・EEE

- link の変化: interrupt-in の endpoint（2 byte、le16 の bit 2 が link の有り。Linux の値）。完了で poll を起こし、link の worker（thread の文脈）が `PLA_PHYSTATUS`（0xe908）を読み、
  bit 1 = link、bit 2/3/4/10/12 = 10/100/1000/2500/5000 Mbit/s。carrier と `link_mbps` を設定。
  - link が上がった時: 2500 以上なら `PLA_MAC_PWR_CTRL4` の bit 6 を下げ、それ以外は上げる（FreeBSD `ure_link_state`）。
  - control 転送は poll の中でなく link の worker（kernel の thread、adapter ごとに 1 本、link の変化と open の時だけ動く）で行う。poll（network worker）の中で数 ms の control 転送で
    待たないため。
  - interrupt の URB が使えない（失敗が続く）時は 1 秒ごとに PHYSTATUS を読む（FreeBSD の tick と同じ形）。
- 自動交渉: OCP の MII の ANAR に 10/100 の全二重・半二重と pause、1000BASE-T の制御（MII 9）に 1000 の全二重・半二重、OCP 0xa5d4（10GBASE-T の制御）の bit 7（2500）、
  RTL8157 は bit 8（5000、Linux の値）、BMCR に自動交渉の開始（FreeBSD `ure_ifmedia_upd` の AUTO）。速さを固定する口は今の stack に無いので自動だけ。
- EEE: device の既定のまま触らない（FreeBSD と同じ）。Linux は EEE を広告するが、遅延の小さい方を選ぶ。数字で差が出れば p003 で試す。
- 速さの log: link の変化ごとに `usb-rtl815x: ue0 link up 2500 Mbit/s full-duplex` / `link down`。

## 8. xHCI の queue depth の使い方

- `drv_usb_endpoint_set_queue_depth`（WS221 p014）で bulk-in に 4（上限 `DRV_USB_ENDPOINT_QUEUE_MAX`）、bulk-out に 4 を頼み、認められた数の URB を作る（1 なら 1 本ずつ）。
  Linux は RX 10・TX 4 本を出す。zedBSD の上限 4 で 48 KB × 4 = 192 KB が device から見た受けの空き。
- xHCI の ring の空き: TRB は 64 KB の境界で分かれ（pci-xhci.c の TD の組み立て）、48 KB の TD は 2 個ほど。4 本と余白でも ring に十分入る（review m5）。
- `drv_usb_urb_reserve_transfer` は使わない: 予約は同期の staging（sync_buffer）を作り、callback 付きの setup を EINVAL で拒む（usb.c の `drv_usb_urb_setup`）。
  review m5 の案で入れた 5534277a3 は 5330 の k1 で open が EINVAL になった（p003）。submit ごとの確保の費用は p003 の数字で見る。
- interrupt-in は depth 1。

## 9. MTU と jumbo

- `mtu_max` = 9000（`NET_MTU_MAX`、RTL8156・8157 とも device は 9 KB の frame まで: Linux `size_to_mtu(9 * 1024)` = 9194、FreeBSD URE_JUMBO_FRAMELEN 9 KB）。
  NCM の mode では 1504 までだった（WS221 p003）ので、jumbo は vendor mode で初めて使える。
- `set_mtu`: 開いている時も受ける。RMS と EARLY_SIZE と（8156B の）TX FIFO の値を control 転送で書き直す（ioctl の文脈は thread）。buffer の大きさは MTU に依らない（RX 48 KB・TX 16 KB）。

## 10. 試験の道

- host（`plan/tools/rtl815x/`）: frame の純粋な部分を `include/drivers/usb/usb-rtl815x.h`・`src/drivers/usb/usb-rtl815x-frame.c` に出し、ASan・UBSan で:
  RX の block の解析（v1・v2、揃え、FCS を除く、最後の frame、長さの壊れ（大きすぎ・小さすぎ・block を越える）で残りを捨てる、checksum の bit の読み）、
  TX の block の組み立て（v1・v2、揃え、16 KB に入る数、入らない frame を残す、checksum の指示と offset の上限）、version の表、PHYSTATUS の速さ、register の dword の shift と byte enable。
- build: amd64・pcat・rpi4・rpi5・zybo・VF2・5330（`config-5330-jumbo.mk`）・RTL8127 の config の vmunix の warning 0。
- QEMU: Realtek の USB の NIC の模型が無い。usb-net（ECM）・usb-storage の非退行（USB core の選び分けの変更が他の device に効かない事）だけ。
- 実機 5330（新しい `plan/tools/rtl815x/5330-rtl.sh`、`5330-ncm.sh` の形、image の build は firmware の license を `yes | make firmware-licenses` で通してよい（2026-10-30 ユーザー
  「試験に関しては私はすべてのライセンスに同意しておりますので、自動でいいです」））:
  - attach の行（chip・version・mac・urbs）、link 2500、ifconfig ue0、tcp-perf send・recv 30 s（相手 10.0.10.2）、ping 1472、MTU 9000 の ping（相手が 9000 の時だけ、
    相手の無い時は未実施）、RX errors・dropped、`usb.rtl815x=off` の起動で NCM に戻る事（A/B）、内蔵の USB（Broadcom の ccid・AX211 の bt・camera）の非退行、camcap。
  - 抜き差し: 5330 は遠隔で抜けないので、`drv_usb_device_reset` 相当の再列挙を guest から起こす口が無ければ未実施として記録し、ユーザーの拾える試験の一覧へ（Q1）。
- RTL8157: host の v2 の試験と build だけ。実機はユーザーの購入の後。

## 11. Phase の分け方（ws.md の表に足す）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | 実装: register の読み書き、RTL8156・8156B の初期化・up・down、RX・TX の block（v1）、checksum、link の worker、選び分け（match 120・CONFIG・boot の token）、host 試験、全 platform の build | p001 |
| p003 | 5330 の実機: send・recv 30 s、NCM と Linux との比べ、非退行、調整（RX block・EARLY・URB の数・EEE、§5.4 の候補） | p002 |
| p004 | RTL8157（資料だけ）: v2 の記述子、Linux の `r8156_init`・`rtl8156_up` の version 16 の枝と `r8157_hw_phy_cfg` の値、5000 の広告、host 試験、build | p002 |
| p005 | 規約の全文の見直し | p002〜p004 |

## 12. design-reviewer の指摘と反映（2026-10-30）

review: 致命 1・重大 4・中 9・軽微（[phase001](phase001/phase.md) に要約）。実装（p002）は上の本文を次のとおり直した形:

| 指摘 | 反映 |
| --- | --- |
| F1 TX の L4 の offset の起点 | §6.3 を直した。`rtl_tx_prepare` は `storage + l3_offset − data` と IPv4 の IHL で frame の頭からの offset を出す。headroom の無い・header の見えない frame、60 byte 未満の frame（device が詰める）、offset が上限を超える frame は `packet_buf_checksum_finish` で software |
| M1 FreeBSD の MCU 種別の誤り | `INDICATE_FLAG`・`SUSPEND_FLAG`・`EXTRA_STATUS`・`CONFIG34`・`RSTTALLY` は PLA（Linux の値）。出典の comment に書いた |
| M2 register の直列化 | adapter ごとの mutex（`register_lock`、LOCK_RANK_DEVICE）で open・close・set_mtu・worker の register の列を囲む。attach は worker と net device を作る前に一人で行う。順序は mutex → adapter の spin lock |
| M3 worker と detach | worker は `kthread_create`（usb-bt と同じ形）、interrupt の完了は worker を起こすだけ。worker は INTR_LINK の bit を前の link と比べ、違う時（と open の時）だけ PHYSTATUS を読む。close は transfer を止めてから mutex の中で MAC を止め、worker は `opened` の時だけ RE・TE を立てる。detach: ready=0 → stop（URB の cancel・drain）→ worker を join → `net_device_gone` → 解放。1 秒の poll の代わりは置かない（interrupt が失敗した時は link を読み直す） |
| M4 merge で既定が vendor mode に | 既定は Q1 の指示（2026-10-30「amd64 の USB を持つ config は y、他は n」）。p003 の 5330 の試験が通るまで branch を merge しない事を Q1 に頼んだ。逃げ道は boot の token `usb.rtl815x=off` |
| m1 0x7420 | Linux と同じく 0x7410・0x7420 で flash の読み込みを待つ |
| m2 RTL8157 の EARLY_SIZE /16・IDLE_SPDWN | EARLY_SIZE は世代 2 で /16（実装済み）。IDLE_SPDWN を飛ばすのは p004 で |
| m3 link up の RE・TE、自動交渉 | link up で RE・TE を立て直す（開いている時）。自動交渉の開始は attach の一度だけ（open のたびに link を落とさない） |
| m4 interrupt の buffer | endpoint の wMaxPacketSize の大きさで受け、先頭の le16 を読む |
| m5 xHCI | ring の空きは十分（§8）。reservation は callback と両立せず、入れて外した（§8、p003 の k1） |
| m6 範囲の外の chip | ID の表を 0x8156・0x8157 に絞る。拒否は log に version と token を出す。core に configuration の前の probe の口を足すのは範囲の外（記録だけ） |
| m7 登録の場所 | pcat.c の NCM の隣（amd64・pcat）。arm64 の platform は既定 n なので今は足さない |
| m8 開いたままの MTU | RMS・EARLY_SIZE・（8156B の）TX FIFO を書き直すだけ。p003 で開いたままの変更と ping を試す。問題が出たら disable → enable の形へ |
| m9 試験 | 5330-rtl.sh に down・up の後の ping、開いたままの MTU の変更を入れた。send と recv の同時、port の上げ下げは p003 で。Linux の基準の firmware の有無は p003 で dmesg を見る。試験の config は `plan/tools/rtl815x/config-5330-rtl.mk` |
| 軽微 | ZLP は `DRV_USB_URB_ZERO_PACKET`。boot の token は一度だけ読む。Linux の版は ref の tree の commit c339f4010（7.3 の開発版）。register・bit の名前は FreeBSD・Linux と違う独自の名前にした。PHY の firmware は範囲の外。IPV4_CS に IP header の計算を任せる（FreeBSD と同じ）のは p003 の send の成否で確かめる |

## 13. 危険と未決

- FreeBSD の手順だけでは Linux の速さに届かない場合（PHY の patch・firmware の有無）。p003 で数字を見て §5.4 を試す。
- 範囲の外の chip に config 1 が選ばれて使えなくなる危険: ID の表を 0x8156・0x8157 に絞り、逃げ道の token を用意する。
- 抜き差し・suspend の実機の確かめが 5330 で難しい（遠隔で抜けない）。
- Linux の値の引用は GPL の file からのデータ。guardrail の規則（block ごとの出典、code・名前は写さない）で扱い、p005 で類似を見直す。
