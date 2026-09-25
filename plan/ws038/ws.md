<!-- awesome-plan project=zedbsd record=ws038 -->

# WS038: Intel Arc dGPU対応

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: なし
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: 番号の予約のみ（Intel Arc dGPU）
<!-- awesome-plan-current:end -->

## 単一目標

Intel Arcの単体GPU（dGPU）のドライバを実装する。

## 担当

別契約のエージェントが担当する見込み（2026-09-23ユーザー指示）。このファイルは番号の予約として作った。範囲・受け入れ条件・Phaseは、担当するエージェントが計画する。

## 前提（参考）

- GPUの共通層は `src/drivers/gpu/`（drv_gpu）、既存のbackendはVenus（WS014）とi915（WS029・WS031）。
- 標準Vulkan libraryはWS030（`userland/base/libvulkan/`）。
- 規約とGuardrailは `plan/guardrail.md`、運用は `plan/master.md`「実行体制とQueue運用方針」。
  デバイスドライバには設計Phaseを入れる。HALの変更は差分ごとの事前承認が要る。

## Phase一覧

未作成。
