<!-- awesome-plan project=zedbsd record=ws048 -->

# WS048: Raspberry Pi 4 の USB（PCIe・VL805 の xHCI・USB キーボード）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG008
Related Milestones: MG003, MG006
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p001（調査と設計）から。2026-09-24 ユーザー判断「後で（今の計画を続ける）」
<!-- awesome-plan-current:end -->

## 目標

実機の Raspberry Pi 4 で USB の Type-A の port が使え、USB キーボードで console に入力できる。

## きっかけ

2026-09-24、実機の RPi4 が login prompt まで起動した（ws044-p009）。ユーザー:「USBが使えないみたいです。コンフィグのせいでしょうか？」

調べた結果、config だけの問題ではない:

| 要るもの | 今 |
| --- | --- |
| BCM2711 の PCIe root complex（brcmstb）の初期化: reset、link の確立、outbound・inbound の window、config 空間の access | 無い。`src/drivers/pci` の PCI の層は PC の ECAM（`pci-pcat.c`）だけ |
| VL805 の firmware の読み込み: PCIe の reset の後に mailbox の `0x00030058`（xHCI の reset の通知）で VideoCore に読ませる | mailbox は HAL の中（`src/hal/arm64/bsp-rpi4/mailbox.c`）にしかない。driver から使うには HAL の口が要る（承認が要る） |
| xHCI（`src/drivers/pci/pci-xhci.c`） | ある。x86 の cache coherent な DMA を前提にしている。Pi 4 の PCIe の DMA は CPU の cache と coherent でないので、ring と buffer の cache の操作か非 cache の memory が要る |
| 割り込み: PCIe の INTx か MSI を GIC の SPI へ | 無い（polling で始める手もある） |
| USB の driver（HID・hub・storage） | ある。rpi4 の config で `n`（PCIe と xHCI が動くまで意味が無い） |

有線 LAN（GENET）も driver が無い。今の実機の入力はシリアルだけ。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| ws048-p001 | 調査と設計: brcmstb の PCIe の初期化の手順（Linux の `pcie-brcmstb.c` の振る舞いを文書と register の説明から。code は写さない）、window と DMA の番地、VL805 の firmware、割り込み、cache の扱い、HAL に要る口（承認が要る差分の案） | planning | — | 設計文書 |
| ws048-p002 | PCIe の root complex の driver と PCI の層の rpi4 の backend: VL805（1106:3483）が列挙される | planning | p001、HAL の承認 | `src/drivers/platform/rpi4` |
| ws048-p003 | xHCI を rpi4 で: VL805 の firmware の読み込み、DMA の cache の扱い、controller の起動と port の検出 | planning | p002 | `src/drivers/pci/pci-xhci.c` |
| ws048-p004 | USB の hub と HID（キーボード）を rpi4 で有効にし、console に入力する | planning | p003 | config、`src/drivers/usb` |
| ws048-p005 | 変更した source の全文の規約確認と、QEMU と実機の回帰 | planning | p002〜p004 | — |

注: QEMU の raspi4b は PCIe を持たないので、p002〜p004 の確かめは実機が中心になる（ユーザーの手を借りる。シリアルがつながると速い）。
