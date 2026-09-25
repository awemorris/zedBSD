<!-- awesome-plan project=zedbsd record=ws044p008 -->

# ws044-p008: 実機の起動の診断（LED・テスト模様・SCTLR）と boot 設定の洗い直し

Phase ID: `ws044-p008`
Parent: [WS044](../ws.md)
Status: cleared（原因が分かった。HDMI1 での確認と UART は実機の結果待ち、2026-09-24）
Queue: ユーザーの指示による割り込み。

## 報告（2026-09-24 ユーザー、ws044-p007 の image で）

「実機、だめでした。表示はされるけど、画面の左30%くらいにゴミが表示されるだけです。VRAMには書き込めているが、何かしらのLCDコントローラの初期化ができていないです。
フレームバッファ設定だけでなく、LCDC周りのブートローダ設定とかを洗い直してもらえますか。」

## 洗い直した結果

- boot partition（FAT16、MBR の 1 番）: `start4.elf`・`fixup4.dat`（対）、`bcm2711-rpi-4-b.dtb`、`overlays/disable-bt.dtbo`、`config.txt`、`vmunix`。欠けは無い。
- `config.txt`: `arm_64bit=1`、`kernel_address=0x80000`、`enable_gic=1`、`enable_uart=1`＋`disable-bt`（PL011 を GPIO14/15 へ）、1080p60 の固定（p007）。
  kernel の UART の分周は 48 MHz を前提にしていたが config.txt に無かったので `init_uart_clock=48000000` を足した。
- mailbox の framebuffer の要求（物理・仮想の大きさ、depth 32、pixel order、allocate、pitch）は一般的な bare-metal の手順と同じ。応答の番地は `& 0x3fffffff` で ARM の物理へ。
- 見つけた危うい点: EL2 から EL1 へ降りるとき `SCTLR_EL1` を設定しておらず、後で M・C・I を OR するだけだった。実機では reset 値が定まらない bit がある（QEMU は定まる）。
- 分からない点: 実機で kernel が framebuffer の処理まで届いているか。届いていなければ、見えているゴミは firmware 自身の framebuffer（kernel が書いていない）。

## 変更（HAL、承認済み）

差分 `plan/ws044/proposed/rpi4-boot-diagnostics.diff`（承認 2026-09-24「承認、すぐ進める」）:

- `led.c`/`led.h`（新規）: ACT LED（GPIO42）で段階を点滅の回数で示す。1 回 = C に入った、2 回 = FDT を読めた、3 回 = framebuffer を確保できた（できなければ 6 回）、4 回 = kernel 本体へ移る直前、その後は点灯のまま。
- `framebuffer.c`: 確保の直後に 3 秒のテスト模様（左から赤・緑・青、白い枠 16 画素）と、大きさ・pitch・order・番地の表示（UART と画面）。
- `locore.S`: EL2 と EL1 の入口で `SCTLR_EL1 = 0x30d00800`（MMU・cache 無効、little-endian）、I-cache の無効化。

## 検証

| 検証 | 結果 |
| --- | --- |
| rpi4 の image の build | 成功（`build/rpi4-font/hdd-image.img`） |
| QEMU raspi4b の boot test | login prompt |
| 実機 | ユーザーの確認待ち（LED の点滅の回数、テスト模様の見え方、UART の log。ユーザーは USB シリアルをつなげる） |

## 実機の結果 1（2026-09-24 ユーザー）

「Blinkは4回です。シリアルには何も出ないですねえ。」 段階 4（kernel 本体へ移る直前）まで届いている。3 の後に 6 ではないので framebuffer の確保は成功、UART への書き込みも止まっていない。
UART の番地（PL011 0xfe201000、GPIO 0xfe200000、GPIO14/15 を ALT0）は正しい。配線の確かめのため `uart_2ndstage=1`（firmware が自分の起動の log を同じ UART に出す）を config.txt に足した。
画面の見立て: Pi 4 の micro-HDMI は 2 つあり、mailbox の framebuffer は既定で display 0（HDMI0、USB-C の隣）に付く。monitor が HDMI1 につながっていると、そこには firmware の framebuffer が出たままになる。確かめをユーザーに頼んだ。

## 実機の結果 2 と原因（2026-09-24）

ユーザー: monitor は HDMI1。LED は 1・2・3・4 と出て点灯のまま。テスト模様は出ない（ゴミのまま）。**HDMI0 につなぎ替えると表示された。**

原因: config.txt の `hdmi_force_hotplug=1`（port の指定が無いと HDMI0 に効く）が、何もつながっていない HDMI0 を display 0 にしていた。
mailbox の framebuffer は display 0 に付くので kernel の描画は HDMI0 へ行き、HDMI1 の monitor には firmware の framebuffer（誰も書かない）が出ていた。
kernel・HAL の framebuffer の処理そのものは正しく動いていた。

直し（HAL ではない）: `hdmi_force_hotplug` を除き、実際につながった display が display 0 になるようにした。1080p60 の指定は `hdmi_group:0/1`・`hdmi_mode:0/1` で両方の port に。
image `build/rpi4-font/hdd-image.img`、QEMU raspi4b で login。HDMI1 での確認は実機の結果待ち。

残り:
- UART に何も出ない。kernel の UART への書き込みは止まっていない（止まれば段階 1 の後で止まる）ので、配線か adapter の疑い。`uart_2ndstage=1` で firmware の log も同じ UART に出るようにした。
- 診断（LED の点滅と 3 秒のテスト模様で起動が約 10 秒延びる）は、実機が安定したら外す（HAL の差分になるので承認を得る）。
