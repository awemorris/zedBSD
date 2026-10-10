# ws157-p009: 今回UAT変更の最終規約と検証

Parent: [../ws.md](../ws.md)
Status: cleared
Disposition: normal
Queue: [media-uat-20261010](../../ws197/codex-queue.md)
Approval: 最新ユーザーの5項目。
Scope / Criteria: 上記有限Queue全sourceをC全文/manual/format/style-check、短いhost試験とnamed build warning0で確認。既存clearance履歴を保持し新変更のみ判定。実機UIはユーザー確認。
Prerequisites: ws197-p012、ws127-p013、ws157-p008
Standards: [C全文](../../coding-style.md)、[Guardrail](../../guardrail.md)、[automation](../../standards/automation.md)。例外なし。
Verification: 変更箇所の短いhost確認、named build warning0、全文manual/format/style-check、diff-check。実機の新規deploymentとUATは別、QEMUなし。

## 2026-10-10 実装と検証checkpoint

今回変更sourceを全文規約/manual/changed-range formatで確認し、短いhost probeとnamed build warning0を通した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)。Phoneの起動SIGSEGVと大きなJPEGのGPU allocation failureは実機で旧/修正版を照合。Photosのprivate worker metadata取得/終了も実機exit0。今回のsource/main統合とscoped clearanceを次に行う。未実施のGUI操作を実機成功にはしない。同期公開/shared boardsの投影はQ1へ保留。

## 2026-10-10 scoped clearance / main read-back

Outcome: cleared。有限Queueの実装・短いhost確認・named build warning0・変更sourceの全文規約確認を完了。source `e654733f1eb6e1296eacf34b5dff6002a5dd17e1` をmainへfast-forwardしHEAD/clean状態を読み返した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)に実機成功と未実施UATを区別。Phone起動と提供JPEGの実機確認は成功。実機GUI操作はユーザーUATとして保持し、WS全体の受入や既存未完Phaseをclearしない。shared boards/cacheへの投影とGitHub公開はQ1側の残件、remote Issue closureは未実施。
