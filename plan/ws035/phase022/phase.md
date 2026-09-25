<!-- awesome-plan project=zedbsd record=ws035p022 -->

# ws035-p022: 設計: hda ドライバ

Phase ID: `ws035-p022`
Parent: [WS035](../ws.md)
Status: **cleared**（q341-i01、2026-09-24）
Phase disposition: normal
Queue: q341（q341-i01）
実行: メインセッション（設計と敵対的レビューを同じセッションで行った。サブエージェントは使わない方針）

## 成果物

[hda-design.md](../hda-design.md)。ソースは変更していない。

要点:

- `src/drivers/pci/pci-hda.c`・`include/drivers/pci/pci-hda.h`・`CONFIG_DRIVER_PCI_HDA`。class 0x0403 の controller ごとに1つの audio device。
- CORB/RIRB を poll で使う。codec は Intel の HDMI・DP（vendor 0x8086）を飛ばし、出力の経路が見つかった最初の analog codec。
- 経路は pin から DAC へ、ADC から pin へ、接続一覧を深さ 5 までたどる。同じ DAC の出力 pin は全部有効にする（jack 検出なし）。
- 音量は DAC に一番近い段数つきの出力 amp。無ければ `ENOTSUP`。
- BDL は fragment ごとに1 entry、IOC つき。位置は LPIB。
- **p006 の interface を変える**: DMA device を backend が `drv_audio_register` に渡す（`drv_pci_device_dma()`）。p007 で実装する。
- Intel PCH の snoop 強制（`DEVC` bit 11）は p008 の確認項目。

## 敵対的レビュー

設計の §9 に 12 件（H1〜H12）を記録し、すべて設計に反映した。大きいのは H1（DMA device の出どころ）、
H2（stop の約束を守れない chip）、H3・H4（QEMU の wav は入力を持たず、`mixer=on` は bit 一致を壊す）。

## 次

p007（実装、QEMU）。p008（実機、人）は p007 の後。
