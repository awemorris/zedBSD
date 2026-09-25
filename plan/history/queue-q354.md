<!-- awesome-plan project=zedbsd record=queue-q354 -->

# Queue q354: 合成の設計（ws035-p051）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「デスクトップのコンポジットは、設計をください」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q354-i01 | [ws035-p051](../ws035/phase051/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q354-i01 | ws035-p051 | **cleared**。[compositing-design.md](../ws035/compositing-design.md) を提出（Vulkan の quad で合成、zdesktop が自分で scanout、直接 scanout を残す、`wl_shm`、acquire fence、damage は 2 段）。承認をもらいたい点 7 つ。実装の Phase p052〜p055 は承認待ちの提案 |
