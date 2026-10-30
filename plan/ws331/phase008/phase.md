<!-- awesome-plan project=zedbsd record=ws331-p008 -->

# ws331-p008: Raspberry Pi の共通化と ODROID-C5 の分離（driver と HAL）

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1721（P2）

## 目的と範囲

2026-10-30 ユーザーから次の趣旨の指示を受けた: driver の Pi 4・Pi 5 の共通の code を `src/drivers/platform/rpi-common/` に、HAL は `bsp-rpi-common/` に寄せる。ODROID-C5 は Pi と source を共有しない: driver は `src/drivers/platform/s905x5m/`、HAL は SoC の共通の `bsp-s905x5m-common/` と board の `bsp-odroidc5/` に分け、bsp の初期化は `bsp-odroidc5/` に置く（同じ SoC の別の board はそれを複製して足す）。ODROID-C5 の board は SoC そのままの機能で、違いは GPIO のピンの配置くらいの想定。SD の clock は handoff から受けてよい。

1. driver: `rpi4-sdhci.c`・`rpi4-console.c` などの Pi 4・Pi 5 の共通の code を洗い出し `rpi-common/` に移す。Pi 5 の SD の 200 MHz の固定は handoff の値で渡す。ODROID-C5 の console の glue（Amlogic の UART）は `s905x5m/` の自分の file に。`HAL_BOARD_*` を driver から無くす。
2. HAL: `bsp-rpi-common/` は Pi だけに。ODROID-C5 の BSP を `bsp-s905x5m-common/`（SoC）と `bsp-odroidc5/`（board、bsp の初期化）に分ける。framebuffer の窓など ODROID-C5 が今 `bsp-rpi-common` から使っている物は、arm64 の共通か s905x5m の側へ。
3. 残りの `HAL_ARCH_*`: overlayfs（全 arch で同じ確保に）、amdrdna・nvrtx の write-combine（kernel の口で使えるかを判断）、pci-xhci の Pi の debug の log（消す）。
4. 確かめ: 20 port の build（warning 0）、QEMU raspi4b の login、Pi 5・ODROID-C5 は build、grep で driver に `HAL_` が無い事。
