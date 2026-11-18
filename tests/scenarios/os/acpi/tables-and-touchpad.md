---
id: os.acpi.tables-and-touchpad
title: ACPI の table が読めて、touchpad が割り込みで読む（Latitude 5330）
status: active
areas: [acpi, i2c, touchpad, gpio]
paths: [src/drivers/acpi/, src/drivers/i2c/, src/drivers/gpio/]
machine: hardware
human: none
since: BUG-195
---

## 目的
BUG-195（DSDT が AML の stack の予算で止まり、touchpad・電池・電源 button が見えなかった）が直ったままであることを確かめる。

## 準備
素の Latitude 5330 を AAT の image で起動した。

## 操作と確認
1. 操作: kernel の log を読む。
   確認事項: namespace。正解: `acpi: N tables listed, namespace ready`。確認方法: grep `namespace ready`。
2. 操作: 止まった table を探す。
   確認事項: table の読み込みの失敗。正解: `ACPI: … stopped at offset …` も `ACPI: … did not load` も無い。確認方法: grep。
3. 操作: touchpad の driver の行を探す。
   確認事項: 割り込み。正解: `i2c-hid: \_SB.PC00.I2C1.TPD0 reads on its interrupt (\_SB.GPI0 pin 327)`。確認方法: grep `i2c-hid:`。
4. 操作: 割り込みの取りこぼしを探す。
   確認事項: 失敗の行。正解: `intel-gpio: irq N fired for no pad`、`i2c-hid: … samples its input`・`did not start`・`the interrupt was given up` が無い。確認方法: grep。

## 合格
namespace ready、止まった table が無い、touchpad が割り込みで読む。

## 注記
指が効くかは desktop.touchpad.gestures（人の手）。
