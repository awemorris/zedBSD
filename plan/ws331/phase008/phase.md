<!-- awesome-plan project=zedbsd record=ws331-p008 -->

# ws331-p008: Raspberry Pi の共通化と ODROID-C5 の分離（driver と HAL）

Status: cleared（2026-10-30 Q1）
Parent: [WS331](../ws.md)
Queue: q1721（P2）

## 目的と範囲

2026-10-30 ユーザーから次の趣旨の指示を受けた: driver の Pi 4・Pi 5 の共通の code を `src/drivers/platform/rpi-common/` に、HAL は `bsp-rpi-common/` に寄せる。ODROID-C5 は Pi と source を共有しない: driver は `src/drivers/platform/s905x5m/`、HAL は SoC の共通の `bsp-s905x5m-common/` と board の `bsp-odroidc5/` に分け、bsp の初期化は `bsp-odroidc5/` に置く（同じ SoC の別の board はそれを複製して足す）。ODROID-C5 の board は SoC そのままの機能で、違いは GPIO のピンの配置くらいの想定。SD の clock は handoff から受けてよい。

1. driver: `rpi4-sdhci.c`・`rpi4-console.c` などの Pi 4・Pi 5 の共通の code を洗い出し `rpi-common/` に移す。Pi 5 の SD の 200 MHz の固定は handoff の値で渡す。ODROID-C5 の console の glue（Amlogic の UART）は `s905x5m/` の自分の file に。`HAL_BOARD_*` を driver から無くす。
2. HAL: `bsp-rpi-common/` は Pi だけに。ODROID-C5 の BSP を `bsp-s905x5m-common/`（SoC）と `bsp-odroidc5/`（board、bsp の初期化）に分ける。framebuffer の窓など ODROID-C5 が今 `bsp-rpi-common` から使っている物は、arm64 の共通か s905x5m の側へ。
3. 残りの `HAL_ARCH_*`: overlayfs（全 arch で同じ確保に）、amdrdna・nvrtx の write-combine（kernel の口で使えるかを判断）、pci-xhci の Pi の debug の log（消す）。
4. 確かめ: 20 port の build（warning 0）、QEMU raspi4b の login、Pi 5・ODROID-C5 は build、grep で driver に `HAL_` が無い事。

2026-10-30 Q1 から範囲の追加（ユーザーの指示ではなく Q1 の判断）: git mv の一覧の OK と 2 つの条件（arm64 の共通に移した file に Pi に固有の分岐・値を残さず、残る物は Pi の側の hook に。git mv は中身の変更と別の commit に）。他の WS の試験（ws206・ws215・ws290）と tree の tests、docs の path の追従を範囲に含める。rpi-sdhci.c の direct map の番地の直書きを kernel の口に置き換え、HAL の memory の配置を直に書く driver を grep で一覧に（直すのは rpi-common と s905x5m だけ）。

## 決めた形（P2、q1721）

- driver（`src/drivers/platform/rpi-common/`）: Pi 4・Pi 5 が共に build する 7 組を `rpi4/rpi4-*` から `rpi-common/rpi-*` へ（sdhci・console・mmc-irq・sdio-host・sdio・gpio・bt-link）、symbol は `rpi4_`→`rpi_`・`RPI4_`→`RPI_`。
  - `rpi-console.c` は Pi 4・Pi 5 だけ。board は引数（`enum drv_rpi_board`、`DRV_RPI_BOARD_PI4`・`PI5`）で、UART の番地と画面の handoff の名前を driver の中の表から選ぶ。
  - `rpi-sdhci.c` は base clock を引数で受ける（0 は capabilities の値）。Pi 5 は HAL が handoff `rpi5.sdhci.clock`（200 MHz、`bsp-rpi5/board.h` の `ARM64_BOARD_SDHCI_CLOCK_HZ`、`boot.c` は board.h がその macro を持つ時だけ返す）を出し、`src/kern/platform/rpi5.c` が読んで渡す。register は direct map の番地（0xffff000000000000 の直書き）でなく `kern_device_map`（1 page、UNCACHED）で map し、attach の失敗では外す。
- ODROID-C5: `src/drivers/platform/s905x5m/s905x5m-console.c`・`.h` を新しく（Pi と共有しない）。debug UART の番地と画面の名前は `src/kern/platform/odroid-c5.c` が渡す（UART は board の選択）。`platform/arm64/s905x5m.mk` の SoC の list に入れた。
- build の macro: driver から `HAL_BOARD_*`・`HAL_ARCH_*` を無くした。serial console の `-DRPI4_SERIAL_CONSOLE` は `-DARM64_SERIAL_CONSOLE` に改名（Makefile、HAL の cons.c、rpi-console.c・s905x5m-console.c・rpi4-bt.c）。HAL の arm64 は `HAL_BOARD_*` を使っていない（残る利用は kernel の vfs.c・klog.c）。
- HAL（hal.h は不変）:
  - board に依らない物を `src/hal/arm64/` 直下へ: boot.c clock.c clock.h cons.c fdt.h font.c font.h framebuffer.c framebuffer.h gic.c gic.h led.c led.h uart.h。symbol は `rpi_`→`arm64_`・`RPI_`→`ARM64_`（`rpi_mailbox_property` と `RPI_BOARD_MAILBOX_BUS_ALIAS` は Pi のまま）。comment の Pi の記述を board に依らない形に。
  - `bsp-rpi-common/` は Pi だけ（mailbox.c・mailbox.h・framebuffer-mailbox.c）。framebuffer の取得は board の hook `arm64_bsp_framebuffer_init(void)`（bsp.h）にし、Pi の版が `arm64_boot_info()->mailbox_base` を読む（cmain が mailbox の番地を渡す形をやめた）。fdt.h の `mailbox_base` は「firmware に mailbox がある board の物」と書いて残した。
  - `bsp-s905x5m/` を `bsp-s905x5m-common/`（SoC: uart.c・fdt.c・framebuffer-boot.c・locore-board.h・soc.c = PSCI・core の番号・reset・poweroff・watch の compositor 無し）と `bsp-odroidc5/`（board: board.h・activity-led.c・board.c = `arm64_bsp_map_devices`、bsp の初期化）に分けた。include path は board の dir の次に SoC の dir（`ARM64_SOC_DIR_<board>`、`platform/arm64/vmunix.mk`）。同じ SoC の別の board は `bsp-odroidc5/` を複製する。
- 範囲 3: `overlayfs.c` は全 arch で inode の表を `kern_calloc` で確保（static の .vfs_bss の版を削除）。amdrdna・nvrtx の write-combine は新しい kernel の口 `vm_device_write_combine_supported()`（`include/kern/vm-device.h`、`src/kern/vm/device.c`）で判断。`pci-xhci.c` の Pi の debug の log 6 か所を削除。

## 結果（2026-10-30）

- commit（基点 main e51965db1）: 74d6760d2（範囲 3）、c95f1c4f6（driver の git mv だけ）、52910a04e（driver の中身・sdhci の clock の handoff・ARM64_SERIAL_CONSOLE・他 WS の試験の追従）、aab1df180（HAL の git mv だけ）、4c1c9c3d7（HAL の中身・makefile・tests の fdt の host 試験・docs の path）、ae0b1b031（rpi-sdhci の kern_device_map）。
- build: `plan/ws331/tests/build-vmunix-all.sh build/p008all`（ae0b1b031）で 20 port の vmunix が全部 rc=0・warning 0。
- grep: `grep -rn 'HAL_\|hal_\|hal/' src/drivers include/drivers` が 0 行。
- host 試験 PASS: `make … rpi4-fdt-host-test`・`rpi5-fdt-host-test`（2 つの DTB）・`odroid-c5-fdt-host-test`、`plan/ws206/tests/gpio-host-test.sh`・`sdio-host-test.sh`（DTB は build/p007-end/bcm2711-rpi-4-b.dtb を引数で）、`plan/ws215/tests/rpi4-bt-host-test.sh`、`plan/ws290/tests/text-end-boards-host-test.sh`（rpi の scene を `drv_rpi_console_init(DRV_RPI_BOARD_PI4)` に）。
- QEMU raspi4b（ae0b1b031）:
  - `plan/ws331/tests/end-rpi4-qemu.sh build/p008-end`（image は config-rpi4-end.mk）: PASS。画面の読み取りで owner の画面 → login → 描き直した login、gdbstub で end の後の HAL の窓 state=3・L0[2]=0・窓が読めない・ticks が進む。login-after.png を P2 が目で見た（SD の mmcblk0 から root を mount し login: まで）。PNG は build/tmp/end-rpi4.XYs7N1/。
  - serial（config-rpi4-serial.mk、ARM64_SERIAL_CONSOLE）: `plan/tools/guest/rpi4-serial.sh build/p008-serial/hdd-image.img 'uname -a' 'echo serial-ok'` で serial の login と 2 つの command が通った。

### HAL の memory の配置を直に書いている driver（grep の一覧）

`grep -rnE '\*\)\s*\(?\s*0x[0-9a-fA-F]{5,}|DIRECT_BASE|0xffff0+ULL' src/drivers include/drivers` ほか（direct map・KSEG・番地の cast を探した）:

| file:行 | 書いている物 | 扱い |
| --- | --- | --- |
| src/drivers/platform/rpi-common/rpi-sdhci.c（旧 23・55） | arm64 の direct map の基底 0xffff000000000000 + 物理番地 | 直した（kern_device_map） |
| src/drivers/platform/pc98/graphics/text.c:34-35 | i386 の HAL の text VRAM の窓 0x800a0000・0x800a2000 | 一覧だけ |
| src/drivers/platform/pc98/graphics/display-glyph.c:117 | 同じ窓の CG 0x800a4000 | 一覧だけ |
| src/drivers/platform/pc98/graphics/display-gdc.c:89-92 | graphics VRAM の物理番地を仮想番地として（0xa8000・0xb0000・0xb8000・0xe0000、HAL の恒等の map が前提） | 一覧だけ |
| src/drivers/platform/pcat/graphics/font.c:20 | i386 の HAL の VGA の窓 0x800a0000（PCAT_VGA_APERTURE_ADDRESS の既定） | 一覧だけ |

s905x5m の driver に直書きは無かった。`kern_pmem_to_kernel` などの kernel の口で direct map を使う所（rpi4-pcie.c・rp1.c・s905x5m-soc.c など、comment の "direct map"）は kernel の口なので数えていない。

## 未実施・残り

- Pi 5・ODROID-C5 は build だけ（実機・QEMU なし）。Pi 5 の SD の 200 MHz が handoff 経由で効く事は code の読みだけ。
- overlayfs の `kern_calloc`（約 3 MiB）が amd64 以外の port の起動で取れる事は raspi4b の起動だけで確かめた（overlay の mount が通ったかは見ていない）。他の port は p009 の QEMU の起動で見る。
- `src/hal/arm64/watch.c` の HVS の形は、Q1 の指示で 0e4a71447 で Pi 4 の hook（`bsp-rpi4/watch-hvs.c`）へ移した（記録は [p009](../phase009/phase.md) の「p008 の追加」）。
- `bsp-s905x5m-common/fdt.c` は board の compatible（"hardkernel,odroid-c5"）を SoC の "amlogic,s7d" と並べて持つ。S7D の tree なら通るので、別の board でもそのまま使える。
- 判定は Q1。
