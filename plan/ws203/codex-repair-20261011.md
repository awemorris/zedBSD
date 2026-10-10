# Codex scoped repair Queue / rpi4-initialization-20261011

Status: finished
Owner: Codex / codex/rpi4-sshd
Approval: current userのEthernet/DHCP調査指示とUSB失敗の追加報告「USBもなんかエラーが」「attach failed at controller start (3)」。先行Queueの履歴は変更しない。
Finite scope: 実機で確認されたGENET初期化失敗とVL805 controller start失敗に直結する既存IRQ/DMA/MMIO契約の修正、限定host checks、warning0 arm64 kernel build、最終C全文reviewまで。実機確認はユーザー、main統合は検証済み具体的commitを提示。HAL API変更/toolchain/QEMU/pushなし。

| Attempt | Phase | Scope | Status | Dependencies |
| --- | --- | --- | --- | --- |
| rpi4-initialization-20261011-i01 | [ws203-p004](phase004/phase.md) | GIC triggerとMMIO release | cleared | main6b722f47e |
| rpi4-initialization-20261011-i02 | [ws048-p010](../ws048/phase010/phase.md) | DMA backingのpage rounding | cleared | 現main DMA API |

Graph: main (context) → i01; main (context) → i02; 両者 → 現config kernel build。共通buildは成果を一度に検証。共有投影/remoteはQ1 pending。

## Terminal result

両itemの限定source/build criteria cleared。[証拠](tests/initialization-repair-20261011.md)。現在config kernel warning/error0、旧DMA対照失敗・新DMA成功、GIC/GENET/MMIO host checks PASS。物理受入は未達。main承認・Q1投影待ち、次Queueは自動開始しない。
