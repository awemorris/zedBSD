<!-- awesome-plan project=zedbsd record=ws050 -->

# WS050: USB-C の UCSI driver

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG003
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p001 から。WS049（AML interpreter）が前提
<!-- awesome-plan-current:end -->

## 目標

USB Type-C Connector System Software Interface（UCSI）の driver で、USB-C の connector の状態（接続・向き・電源の役割・データの役割・
Alternate Mode・USB PD の contract）を読み、変化を受け取り、必要な操作（role の切り替え、Alternate Mode の選択）ができる。

## きっかけ

2026-09-24 ユーザー指示: 「USB-C UCSIドライバの実装。」

## 前提と今あるもの

- PC の UCSI は ACPI の device（`_HID` の `USBC000`、`_CID` の `PNP0CA0`）で、共有の memory（OperationRegion）と `_DSM` で EC/PD controller と話す。
  → **WS049 の AML interpreter が前提。**
- USB の xHCI・hub・HID・storage の driver はある（`src/drivers/usb`、`src/drivers/pci/pci-xhci.c`）。Type-C の概念（connector・partner・Alternate Mode）の層は無い。
- 対象機は WS049 と同じ（仮定）。

## 範囲

- UCSI の command（`PPM_RESET`、`SET_NOTIFICATION_ENABLE`、`GET_CAPABILITY`、`GET_CONNECTOR_CAPABILITY`、`GET_CONNECTOR_STATUS`、`GET_ALTERNATE_MODES`、
  `GET_CAM_SUPPORTED`、`GET_CURRENT_CAM`、`SET_NEW_CAM`、`GET_PDOS`、`SET_UOR`・`SET_PDR` ほか）と通知の割り込み（ACPI の `Notify`）。
- Type-C の connector の層（状態、partner の Alternate Mode の一覧）と、`/dev/system` か専用の device での状態の公開。
- DP Alt Mode の入口（WS051 が使う: mode に入る・出る、HPD の通知）。

## 受け入れ

- 対象機で各 USB-C port の抜き差し、向き、電源の役割、partner の Alternate Mode が読め、抜き差しの通知が届く。
- 規約の全文、build、boot test。実機の証拠と QEMU の証拠を分ける（QEMU には UCSI が無いので、試験は実機が中心）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| ws050-p001 | 調査と設計: UCSI の仕様（1.2/2.x）、対象機の `USBC000` の AML（共有 memory の配置、`_DSM` の function）、Type-C の層の設計、公開の形 | planning | WS049 の namespace と評価器 | 設計文書 |
