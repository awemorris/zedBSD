<!-- awesome-plan project=zedbsd record=ws315-p004 -->
# ws315-p004: RTL8157（5G）の初期化（資料だけ）

Status: cleared（2026-10-30 05:43 P3 q1694-i01: host 試験 PASS、amd64・pcat（driver y）の build warning 0。実機は未実施、ユーザーの購入の後）
Parent: [WS315](../ws.md)
Queue: q1694 / q1694-i01（P3）
依存: p002

## 範囲

RTL8157（version 0x1030）を attach で拒まず、vendor mode で上げる。記述子 v2 の解析・組み立て（p002 で実装と host 試験済み）に加え、chip・PHY の初期化、up、link。
出典は Linux 7.3 `drivers/net/usb/r8152.c`（GPL-2.0、FreeBSD 15.0 の if_ure に RTL8157 は無い）の値と手順だけを、block ごとに「Values referenced」「Procedure referenced」で示して使う（code・名前・構造は写さない、design.md §1）。

## 実装（`src/drivers/usb/usb-rtl815x.c`）

| 部分 | 内容 | Linux の出典（値・手順） |
| --- | --- | --- |
| 間接の読み書き | `rtl_indirect_read`・`_write`・`_modify`: USB 0xd5d6（address）・0xd5d8（data、dword）・0xd5dc（command: busy bit 0、write bit 1、IP bit 2、BMU 0）。busy を 2 ms × 10 回まで待つ | `ocp_adv_read`・`ocp_adv_write`・`wait_cmd_ready` |
| PHY の SRAM | `rtl_sram_modify`（OCP 0xa436/0xa438 と 0xb87c/0xb87e、address を書き data を読んで書き戻す）、`rtl_phy_steps_apply`（表を順に）、`rtl_phy_patch_request`（0xb820 bit 4、0xb800 bit 6 を 5 s まで、許されない要求は取り下げる） | `sram_write_w0w1`・`sram2_write_w0w1`・`rtl_phy_patch_request` |
| chip の初期化 | `rtl_chip_init` を chip ごとの最初の設定（`rtl_chip_init_first_8156`・`_8157`）と共通の `rtl_chip_wake`（flash の読み込み・autoload・外部初期化の PHY・BMCR・LAN-on）に分けた。8157: USB 0xcffe bit 3 を上げ 0xd3ca bit 0 を下げ、0xd32a（speed option）は触らない。U2P3 は IP 0xc2c0 bit 29（間接）、power cut は USB 0xcfff bit 1 も下げる。MCU の clock の speed down は触らない。USB_CTRL は RX_AGG_DISABLE と 16 byte の記述子の bit（0x0400）を下げる。BMU 0x2300 bit 3 を下げ、USB 0xd4ae bit 1 を下げ、OCP 0xa5b4 bit 15 を下げる | `r8156_init` の RTL_VER_16 の枝、`r8157_u2p3en`・`r8157_power_cut_en`・`r8156_ups_en`・`r8156_mdio_force_mode` |
| ALDPS | 8157 は OCP 0xa430 bit 2 を下げ、PLA 0xe000 bit 8 を 20 ms まで待つ（8156 は FreeBSD の形のまま） | `r8153_aldps_en` |
| PHY の設定 | `rtl_phy_config_8157`（attach で chip の後に一度）: flash、PCUT_STATUS、外部初期化の 0xa466・0xa468、BMCR、ALDPS を切る、LAN-on、PLA 0xe84c の PFM を下げ、0xa430 bit 0・1、0xa44a bit 2 を下げる、**PHY の parameter の表 99 手**（`rtl_8157_phy_steps`: XG_INRX・green・相手の認識・RFI の閾値と noise の生成 5G/2.5G・熱の speed down の閾値・XG の互換）。`_finish`: patch の要求の中で PLA MAC_PWR_CTRL4 bit 0、OCP 0xa442（EEE の 100・1000 を下げ 10M の clock 分周）、0xa430 bit 15、要求を下ろし、green（SRAM 0x8011 bit 15）、0xa428 bit 9・0xa5ea bit 0・1 を下げ、ALDPS を入れる。PHY の firmware は load しない、EEE は chip の既定のまま（design.md §5.4・§7） | `r8157_hw_phy_cfg`（RTL_VER_16 の枝、値はデータとして表に） |
| up | `rtl_nic_reset_8157`: U1U2・ALDPS を切る、rxdy gated・teredo・受けを止める、MAC の reset でなく RE・TE を下げる、BMU の reset、OOB・PLA 0xc012 bit 3・MCU_BORW を下げる、VLAN を剥がさない、frame の大きさ、TXFIFO_CTRL = 512/64、RXFIFO の閾値、MCU の clock、RX_BUF_TH、ALDPS を入れる。U2P3・U1U2 を入れ直さない | `rtl8156_up`・`rtl8152_nic_reset` の RTL_VER_16 の枝 |
| MTU に従う値 | `rtl_program_frame_fifo_8157`: pause on/off =（frame を 1 KB に切り上げ + 6 KB / 14 KB）/ 16、TXFIFO_FULL =（frame + TX 記述子 16 を 1 KB に切り上げ）/ 16。open と開いたままの set_mtu で書く。EARLY_SIZE は /16（p002 で実装済み） | `fc_pause_on_auto`・`fc_pause_off_auto`・`rtl8156_change_mtu`・`r8153_set_rx_early_size` |
| enable | 8157 は USB 0xd4ee の下 9 bit（RX の集約の frame の数）を下げる | `rtl8156_enable` |
| link | 8157 は 2500 の IDLE_SPDWN（MAC_PWR_CTRL4 bit 6）を触らない | `rtl8156_enable`（RTL_VER_16 は早く返る） |
| attach | 8157 を拒まない。RX block 32 KB、chip の初期化の後に `rtl_phy_config_8157`、5000 の広告（p002 で実装済み） | `rtl_ops_init` の RTL_VER_16 |

- 8156・8156B の道は動作を変えていない（first settings と wake への分割は同じ順・同じ値。8156B の flash の待ちは関数に出しただけ）。

## 確かめ

- host: `OUT=build/ws315-p004-frame sh plan/ws315/tests/run-rtl815x-frame-test.sh` → `rtl815x-frame: PASS`（-O2）と `rtl815x-frame: PASS`（ASan・UBSan）。v2 の記述子・version 0x1030 → RTL8157・RTL8157 の形は p002 の試験が覆う。register の手順は host の模型が無く試験していない。
- build（vmunix、warning 0）: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws315-p004-amd64 build/ws315-p004-amd64/vmunix`（amd64 vmunix check PASS）、
  `make ZEDBSD_CONFIG=config/ci/config-pcat.mk CONFIG_DRIVER_USB_RTL815X=y BUILD=build/ws315-p004-pcat build/ws315-p004-pcat/vmunix`（i386 で driver を compile、PC/AT vmunix contract OK）。
  他の platform（rpi5・zybo・VF2）は driver が n で、変えた file（usb-rtl815x.c）を compile しない。`git diff --check` OK。
- PHY の表は Linux の RTL_VER_16 の枝から script で値を抜いた（99 手、手で写していない）。
- 未実施: RTL8157 の実機（手元に無い。ユーザーの購入の後）。8156B の 5330 の回帰（p003 の image の試験と合わせて、5330 の SSH の承認の後）。

## 結果

- 資料だけの実装・host 試験・build の受け入れを満たした。実機で動くかは未確認（ユーザーの購入の後の試験の一覧へ: Q1）。
