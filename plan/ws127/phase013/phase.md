# ws127-p013: double-tap dragの誤open修正

Parent: [../ws.md](../ws.md)
Status: in-progress
Disposition: normal
Queue: [media-uat-20261010](../../ws197/codex-queue.md)
Approval: 最新ユーザーの5項目。
Scope / Criteria: Filesで2回目のpressにopenせずreleaseに判断する。drag開始/閾値超過時はopenを取り消す。通常double-clickとselection/DnD維持。
Prerequisites: 現main Files DnD
Standards: [C全文](../../coding-style.md)、[Guardrail](../../guardrail.md)、[automation](../../standards/automation.md)。例外なし。
Verification: 変更箇所の短いhost確認、named build warning0、全文manual/format/style-check、diff-check。実機の新規deploymentとUATは別、QEMUなし。

## 2026-10-10 実装と検証checkpoint

今回変更sourceを全文規約/manual/changed-range formatで確認し、短いhost probeとnamed build warning0を通した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)。Phoneの起動SIGSEGVと大きなJPEGのGPU allocation failureは実機で旧/修正版を照合。Photosのprivate worker metadata取得/終了も実機exit0。今回のsource/main統合とscoped clearanceを次に行う。未実施のGUI操作を実機成功にはしない。同期公開/shared boardsの投影はQ1へ保留。
