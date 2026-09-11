<!-- awesome-plan project=zedbsd record=ws004-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws004/phase001/phase.md`

親: [ws004](https://github.com/awemorris/zedBSD/issues/5)

# WS004 Phase 001: PCIe/DMA/interrupt/xHCI foundation audit

Last updated: 2026-08-25

Phase ID: `ws004-p001`

Status: complete (software audit; hardware observations carried forward)

Parent: [WS004](https://github.com/awemorris/zedBSD/issues/5)

Audit: [foundation audit](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/phase001-foundation-audit/audit.md)

Tests: [WS004 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/tests/README.md)

## Objective

Establish the current PC/AT hardware-foundation baseline, fix defects that are
safe without the target laptop, and separate QEMU/host evidence from findings
that require the Dell Latitude 5320.

## Scope

- PCI configuration, topology, capabilities, BAR and interrupt support;
- DMA constraints and mapping behavior;
- amd64 interrupt-controller support;
- USB host-controller coverage and the xHCI gap.

Implementing PCIe ECAM, MSI/MSI-X, scatter/gather DMA, IOMMU, or xHCI is out of
scope. Those are implementation Phases selected from this audit.

## Work packages

- [x] Inventory the relevant source paths and supported mechanisms.
- [x] Record capability and lifecycle gaps without inferring target hardware.
- [x] Make rescanning a PCI bus idempotent instead of duplicating devices.
- [x] Define and enforce DMA segment-boundary constraints.
- [x] Add focused host regressions for both fixes.
- [x] Rebuild the configured amd64 system.
- [x] Record hardware-only checks as carried forward to target execution.

## Completion conditions

- The audit identifies the present PCI/DMA/interrupt/USB capabilities and
  omissions.
- Safe fixes have focused passing regressions.
- The configured amd64 build passes.
- Unknown target-specific facts are explicitly assigned to `ws003-p001` or a
  later WS004 hardware Phase.

## Result

Complete for the software-audit scope. Both host regressions pass, and
`make -j16` rebuilds and validates the amd64 kernel/image. Physical PCIe
topology, interrupt routing, IOMMU state, and xHCI identity remain unverified
because `ws003-p001` has no Latitude evidence.

## Resume point

Select HW-01 only after defining the PCIe ECAM/MSI prerequisites needed by the
xHCI design. Attach the Latitude inventory when it becomes available; do not
retroactively treat the QEMU/PC-AT findings as physical-hardware results.
