<!-- awesome-plan project=zedbsd record=ws048 -->

# WS048: Raspberry Pi 4 の USB（PCIe・VL805 の xHCI・USB キーボード）

<!-- awesome-plan-current:start -->
Status: incomplete（p008実機boot例外はcleared、p009 USB修正source/build済み・実機入力待ち）
Primary Milestone: MG008
Related Milestones: MG003, MG006
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: [Codex USB/sessiond](codex-usb-session-queue.md) finished。旧PCIe boot/2026-09-27の実行記録は保持。
Resume point: p009の具体的commit統合承認、USB keyboard/mouse実機受入。従来p005〜p007の履歴/未完criteriaは保持。
Target: **ベータ4 以降**（2026-10-05 user「WS037, WS044,WS048,WS141, WS112, WS118, WS124, WS125, WS126, WS119, WS096, WS097, WS039, WS038, WS144, WS143, WS146,WS147, WS152,  WS119, WS080, は、ベータ4以降としてください。…WS027, WS015, WS047, WS028, WS017,  WS077, はキャンセルします。」）
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

## 設計

[design.md](design.md)（ws048-p001）。

## Phase 一覧

2026-09-27 の p001 で分け直した（旧 p002〜p005 は実行前だったので番号を振り直した）。

| Phase | 内容 | Status | 依存 | HAL の承認 |
| --- | --- | --- | --- | --- |
| [ws048-p001](phase001/phase.md) | 調査と設計 | cleared | — | 不要 |
| [ws048-p002](phase002/phase.md) | FDT の reader、arm64 の device mapping の実装の修正、brcmstb の host bridge と PCI の backend（VL805 が列挙される） | cleared（実機は未実施） | p001 | 不要 |
| [ws048-p003](phase003/phase.md) | firmware の mailbox と VL805 の firmware の通知 | cleared（実機は未実施） | p002 | 不要 |
| [ws048-p004](phase004/phase.md) | 非 coherent な DMA（`hal_pmem_map_uncached` と `dma.c`） | cleared（2026-09-27。承認済みの hal.h の差分を適用、host 試験・rpi4 と amd64 の boot test PASS。実機は未実施） | p001 | 済み（Guardrail の表） |
| [ws048-p005](phase005/phase.md) | xHCI を rpi4 で | uncleared（build と glue の準備は済み。有効にするのは p004 の後） | p002・p003・p004 | 不要 |
| [ws048-p006](phase006/phase.md) | USB の hub と HID キーボードで console に入力 | planned | p005 | 不要 |
| [ws048-p008](phase008/phase.md) | ユーザー報告のPCIe最初のMMIO読み出し例外を修正 | cleared（ユーザー実機でlogin到達、2026-10-11） | 現在main/config | API変更なし |
| [ws048-p009](phase009/phase.md) | USB入力のactivationと選択済みclass driverの接続 | uncleared（source/build済み、実機入力待ち） | 現在main/config | API変更なし |
| [ws048-p007](phase007/phase.md) | 規約の全文の確認と回帰、実機の結果の取りまとめ | planned | p002〜p006 | 不要 |

注: QEMU の raspi4b は PCIe を持たない（DTB の PCIe の node を disabled にする）。p002〜p006 の動作の確認は実機だけで、
このリポジトリに実機の試験の仕組みは無い。実機の確認はユーザーに頼み、行うまで「未実施」と書く。

## 2026-09-27 の実行のまとめ（サブエージェント、worktree の branch）

- p001〜p004 cleared（実機の確認は未実施）。p005 は承認を要らない部分まで進め、uncleared（config を有効にするのが残り）。p006・p007 は planned。
- host 試験: `make -f plan/ws048/tests/host-test.mk run DTB=<firmware の bcm2711-rpi-4-b.dtb>`（FDT・brcmstb の register model・mailbox の model・DMA の 2 通り。ASan・UBSan）。
- QEMU raspi4b は PCIe を disabled にするので、QEMU で確かめたのは「起動を壊さない」ことだけ。PCIe・VL805・USB の動作は実機だけ。
- rpi4 の image の `make -j16` はこの branch の起点で userland（`src/rtld/rtld.c` と `include/libc/elf.h` の macro の再定義）で止まる（WS048 の外）。
  boot test は main の `build/ws053-rpi4-full/hdd-image.img`（2026-09-25）を SD にし、kernel だけをこの branch の build にした。

## Future Work の候補（2026-09-27 に plan/future-work.md の F-025〜F-029 へ移した）

| 候補 | 理由・きっかけ |
| --- | --- |
| brcmstb の spread spectrum（`brcm,enable-ssc`、design.md §2.4） | DT が求めるが、link と USB には要らない。実機で link を確かめた後 |
| brcmstb の MSI（SPI 148 の受け口） | INTx で足りる。PCI の core の MSI の経路は HAL の LAPIC 向けの口を使うので、host bridge が address と data を出す形が要る |
| arm64 の `hal_space_unmap_device()` | 今も `hal_space_unmap(HAL_SPACE_SYS, ...)` で失敗を返す。PCIe は対応を外さないので影響は無い |
| HAL の mailbox の FULL の確認の register | `src/hal/arm64/bsp-rpi4/mailbox.c` が mailbox 0 の status（`+0x18`）を読む。正しくは mailbox 1（`+0x38`）。実害は出ていない（design.md §7） |
| 有線 LAN（GENET） | きっかけの報告（2026-09-24）で、有線 LAN の driver も無いと分かった。別の WS |

## 要検討・ブロック（2026-10-05）

ユーザーの指示で要検討の状態にしてブロックする。ユーザーと方針を決めるまで Queue に入れない。

## 2026-10-11 起動例外の限定再開

ユーザーの最新の写真と修正指示でp008/独立Queueを追加。全USB計画を自動で再開せず、このboot例外と関連MMIO mappingのみを修正/build確認する。旧p002〜p004の実機未確認は保持。実機の受入はユーザー確認、共有投影/GitHubはQ1 pending。

p008のsource/host/buildは完了、実機再起動結果がないためuncleared。RAM判定のunsigned減算のunderflowを除きMMIOをDeviceでmapし、PCIe revision読出しをreset/SerDes起床後へ移した。警告0 kernel buildとbefore/afterのhost確認PASS。[証拠](tests/rpi4-pcie-boot-20261011.md)。旧WSのUSB実機acceptanceは未達のまま、具体的commitのmain統合承認とユーザー実機確認を待つ。共有投影/GitHubはQ1 pending。

## 2026-10-11 USB / Graphical login限定修正

ユーザーの指示で[p009](phase009/phase.md)と[独立Queue](../ws048/codex-usb-session-queue.md)を追加。USB入力と選択済みclassのglue、Graphical loginのfirmware command lineを補う。ロゴ/kernel animationは明示延期。過去の実機未確認・WS全体の受入は保持。共有投影/GitHubはQ1 pending。

p008 follow-up: user confirms login and VC4 initialization after main fbcb2b543, so only that boot-exception Phase clears. Original attempt/outcome remains historical, new USB/desktop work is p009; WS048 full physical acceptance stays incomplete.

p009 terminal: HID pending activationと選択classの不足を補完、現在configのkernel/full image buildとimage check、最終差分全文review PASS。USBの現在の実機動作は未確認なのでuncleared。[証拠](tests/rpi4-usb-session-20261011.md)。共有投影/GitHubはQ1 pending。
