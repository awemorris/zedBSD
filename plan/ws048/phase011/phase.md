<!-- awesome-plan project=zedbsd record=ws048-p011 -->
# WS048 p011: VL805 command completion / PCIe DMA address

Status: cleared
Disposition: normal
Parent: [WS048](../ws.md)
Queue: [限定Queue](../codex-command-repair-20261011.md)
Authority: current userのUSB不動作log、Ethernet成功、SSH調査指示（10.0.30.2）。
Scope: PCIe dma-rangesのPCI baseをgeneric DMAのdevice addressへ反映、低いCPU RAM allocationを維持。commandとtransfer doorbellのposted writeをreadbackで完了し、timeoutにUSBSTS/CRCR/IMAN/ring位置を出す。既存HAL宣言・UAPIは変更しない。
Criteria: high PCI DMA alias（4/8GiB）、zero alias、map/vectorとmask/overflowの限定host確認、PCI bridge設定/constraints model、doorbell publication順序review、現在config warning0 arm64 kernel build、全変更C全文準拠review。USB入力の実機受入はp009/p006に残す。
Boundaries: MSI実装/他GPU実装/共通toolchain/共有Master/Queue/cacheは変更しない。SSHは読み取りのみ、remote update/rebootなし。
Standards: AGENTS.md、Guardrail、plan/coding-style.md全文、.clang-format。既存testは未完了WSで現行Phaseが参照するものを限定使用。main統合/GitHub/共有投影はQ1 pending。

## Investigation / 2026-10-11

実機g402598d、8GiB、VL8051106:3483、PCIe gen2 x1、INTx175。controller RUN/port reset成功後Enable Slot(type9)timeout、以後command_failedによるEIO。command_exはIRQを切った直接event pollも行うためINTxだけは根拠にならない。
Generic DMAはdevice addressにCPU paddrをそのまま返すが、brcmstbは非zero inbound PCI baseを許してBAR2に設定している。これらの契約が不一致。actual runtime dma-rangesは現kernelで取得する口がなく未確認なので物理root causeとは断定しない。次kernelはinbound base/sizeのlogを残す。Linux xHCIはdoorbell write直後readbackしposted writesをflushするが現sourceはwriteだけ。hardware factsを参照し独自実装、外部codeの転用なし。

## Terminal source/build result / 2026-10-11

Limited source/build criteria cleared. [Evidence, final hashes, exact commands, versions, transient failures and limitations](../tests/command-repair-20261011.md). DMA/PCIe models PASS with normal sanitizers, current config warning0 kernel build PASS, final changed-source full-standard review complete. The final design adds a window creation API while preserving the original constraints layout and all identity callers; this is internal to the Phase and does not change HAL/UAPI or foreign Phase commitments. Actual USB recovery is unverified and p009/p006 remain uncleared/planned. No remote update/reboot. Main concrete-commit approval and shared projections/GitHub are pending.
