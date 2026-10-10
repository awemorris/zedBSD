# ws197-p012: Phone添付の表示と開く操作

Parent: [../ws.md](../ws.md)
Status: in-progress
Disposition: normal
Queue: [media-uat-20261010](../../ws197/codex-queue.md)
Approval: 最新ユーザーの5項目。
Scope / Criteria: 受信した写真/動画はダブルクリックでImage Viewer/Video Playerに正しい保存pathを渡す。draft画像はサムネイルとファイル名、removeを表示。
Prerequisites: p010/p011実source
Standards: [C全文](../../coding-style.md)、[Guardrail](../../guardrail.md)、[automation](../../standards/automation.md)。例外なし。
Verification: 変更箇所の短いhost確認、named build warning0、全文manual/format/style-check、diff-check。実機の新規deploymentとUATは別、QEMUなし。

## 起動SIGSEGVの追加報告と調査

2026-10-10ユーザー「Phone起動1分待ち」。SSHで現行311283164/121120を正しいWayland環境で起動するとREADY後exit139。fault PC d5a9はPIE bias1000を除くc5a9、view_itemのcaptionなしmediaのitem->text[0]。store再読込は空bodyをNULLとして保持するため、空文字だけの判定がNULLを参照。p012の表示境界内でNULLを空caption扱いに直す。

## 2026-10-10 実装と検証checkpoint

今回変更sourceを全文規約/manual/changed-range formatで確認し、短いhost probeとnamed build warning0を通した。[共通証拠](../../ws157/tests/media-uat-verification-20261010.md)。Phoneの起動SIGSEGVと大きなJPEGのGPU allocation failureは実機で旧/修正版を照合。Photosのprivate worker metadata取得/終了も実機exit0。今回のsource/main統合とscoped clearanceを次に行う。未実施のGUI操作を実機成功にはしない。同期公開/shared boardsの投影はQ1へ保留。
