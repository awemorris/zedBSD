<!-- awesome-plan project=zedbsd record=ws029 -->

# WS029: i915ネイティブGPU実装

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

i915 の native の GPU（Alder Lake-P、5330）の driver の土台: 対象の確定と license の境界、driver の骨格（PCI attach・MMIO・forcewake・GGTT・割り込み）、memory、command の投入と後続の Phase（q314 で p001〜p007 cleared）。

## 制限・移管

後続の課題（cold VFIO attach の間欠の停止など）は登録済みの別の Phase・WS へ。display は WS051・WS113、描画の高速化は WS075（空き時間）。全文規約の見直しはベータ3。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws029-p001 | 対象確定・ライセンス境界・移植方針の固定 | cleared |
| ws029-p002 | driver骨格: PCI attach、MMIO/forcewake、GGTT、割込み | cleared |
| ws029-p003 | メモリ: GEM object、48-bit PPGTT、CPU view | cleared |
| ws029-p004 | 実行: engine/LRC/execlists、request/seqno、engine reset | cleared |
| ws029-p005 | drv_gpu統合、native stream、生UAPI試験クライアント | cleared |
| ws029-p006 | VFIO passthroughテストループ（host手順・harness・初回起動） | cleared |
| ws029-p007 | 実機での描画確認、静的レビュー、規約全文確認 | cleared |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
