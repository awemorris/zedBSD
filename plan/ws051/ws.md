<!-- awesome-plan project=zedbsd record=ws051 -->

# WS051: USB-C の DisplayPort Alternate Mode

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p001 から。WS050（UCSI）と i915 の display が前提
<!-- awesome-plan-current:end -->

## 目標

USB-C の port につないだ DisplayPort の display（USB-C の monitor、USB-C から DP・HDMI への変換）に、DisplayPort Alternate Mode で画面を出す。

## きっかけ

2026-09-24 ユーザー指示: 「USB-C DisplayPort Alternative Modeの実装。」

## 前提と今あるもの

- Alternate Mode に入る・出る、pin の割り当て、HPD は UCSI（**WS050**）を通して PD controller と話す。
- 画面を出すのは GPU の display engine: 対象機（Alder Lake-P）では i915 の Type-C の subsystem（TCSS: FIA の lane の割り当て、Type-C PHY、
  DP の link training、HPD の割り込み）。i915 の driver は WS029・WS031 にある（render と Vulkan）。**display の modeset（pipe・transcoder・DDI）の
  範囲と、内蔵 panel 以外の出力がどこまであるかは p001 で調べる。**
- zdesktop（WS035）の複数 display の扱いは、出力が出た後の話。

## 範囲

- UCSI で DP Alt Mode に入り、pin の割り当て（C・D・E）と HPD を受け取る。
- i915 の TCSS: lane を DP に割り当て、Type-C PHY を DP で使い、DDI・transcoder・pipe を立てて link training、EDID の読み出し。
- 抜き差しと HPD の IRQ の扱い、画面の構成の変更を上（framebuffer・zdesktop）に通知する。

## 受け入れ

- 対象機の USB-C port につないだ DP の monitor に画面が出る（mirror か拡張かは p001 で決める）。抜いて差し直すと戻る。
- 規約の全文、build、boot test。実機の証拠が中心（QEMU には無い）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| ws051-p001 | 調査と設計: DP Alt Mode（VESA）と UCSI の手順、i915 の TCSS・Type-C PHY・DDI の手順（Intel の公開文書から）、今の i915 の display の範囲、画面の構成の通知 | planning | WS050 の p001 | 設計文書 |
