<!-- awesome-plan project=zedbsd record=queue-history q459 -->

# Queue q459: acquire fence（ws035-p054）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q459
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「…ws035を次に優先。…これで進めてください。」。p054 の範囲は 2026-09-25 のユーザーの承認（[compositing-design.md](../ws035/compositing-design.md) の D3）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q459-i01 | [ws035-p054](../ws035/phase054/phase.md) | uncleared（受け入れ 1・2・4 は満たした。3 の「present が速くなる」は測って速くならず、判断待ち） |

依存: ws035-p052（cleared）。

結果: acquire fence（`zed_gpu_buffer_v1` version 2、zwl の poll での待ち、WSI の先の commit）を実装。libc の `setvbuf` を直した（BUG-055）。

追記（2026-09-26）: ユーザー「p054 clearedでいいです。」により受け入れ 3 を読み替え、ws035-p054 は cleared。上の q459-i01 の結果（uncleared）は当時の記録として残す。

Upcoming Work Outlook: ws035-p011（窓管理）、p055（damage）、p057（効果）。
