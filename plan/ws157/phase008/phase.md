# ws157-p008: import操作と大きなJPEGのUAT修正

Parent: [../ws.md](../ws.md)
Status: cleared
Disposition: normal
Queue: [media-uat-20261010](../../ws197/codex-queue.md)
Approval: 最新ユーザーの5項目。
Scope / Criteria: Folder chooserでdirectoryを選択。import/list待ちをworker所有Wayland接続へ移しUI/thumbnail threadと分離。取り込み通知/partial error/close所有を維持。添付4080×3072 JPEGのmetadata/import/decoderを調べ改善。
Prerequisites: p006出力、添付JPEG原本。ws197-p012の関連表示とは独立、最終検証はp009が合流。
Standards: [C全文](../../coding-style.md)、[Guardrail](../../guardrail.md)、[automation](../../standards/automation.md)。例外なし。
Verification: 変更箇所の短いhost確認、named build warning0、全文manual/format/style-check、diff-check。実機の新規deploymentとUATは別、QEMUなし。

## JPEG実機診断と設計

同じ4080×3072原本を実機の一時pathでImage Viewerへ渡すとJPEG decode554msは成功、vkAllocateMemory=-2で終了。i915のI915_MAX_RESOURCE_BYTES=16MiBと約50MiBのtextureが衝突。ドライバ上限を変更せず、Image Viewerでmemory errorのときだけ原本をCPU samplingし、既存のwindow-sized canvasをGPUへ渡す。元画像寸法/原本/1:1 zoom/回転/mip/animationを維持。通常画像は既存GPU経路。ユーザーへ原因と対策を報告。

## 2026-10-10 実装と検証checkpoint

今回変更sourceを全文規約/manual/changed-range formatで確認し、短いhost probeとnamed build warning0を通した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)。Phoneの起動SIGSEGVと大きなJPEGのGPU allocation failureは実機で旧/修正版を照合。Photosのprivate worker metadata取得/終了も実機exit0。今回のsource/main統合とscoped clearanceを次に行う。未実施のGUI操作を実機成功にはしない。同期公開/shared boardsの投影はQ1へ保留。

## 2026-10-10 scoped clearance / main read-back

Outcome: cleared。有限Queueの実装・短いhost確認・named build warning0・変更sourceの全文規約確認を完了。source `e654733f1eb6e1296eacf34b5dff6002a5dd17e1` をmainへfast-forwardしHEAD/clean状態を読み返した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)に実機成功と未実施UATを区別。Phone起動と提供JPEGの実機確認は成功。実機GUI操作はユーザーUATとして保持し、WS全体の受入や既存未完Phaseをclearしない。shared boards/cacheへの投影とGitHub公開はQ1側の残件、remote Issue closureは未実施。
