# Codex メディアUAT修正 Queue

Cycle: media-uat-20261010
Status: finished
Owner: 本Codexセッション、codex/ws197-media-uat、base 624a7301c。共有master/Queue/cacheはQ1。
Approval: 2026-10-10最新ユーザーコメントの5項目を直接承認として記録。Phone添付のダブルクリックで写真はImage Viewer、動画はVideo Playerへ開く。draft画像にサムネイル。Filesのdouble-tap dragによる誤openを修正。Photos Import Folderはfolder選択。添付JPEGのmediastorage/decoder不具合調査・修正。Photos importをbackground threadへ移し操作を継続可能にする。以前のmain統合承認保持。有限scopeは下記4項目、今回sessionでhost/buildまで、実機UATはユーザー。外部送信/不要なdesktop restartなし。

| Attempt | Phase | Scope / Criteria | Status | Dependency |
| --- | --- | --- | --- | --- |
| media-uat-i01 | ws197-p012 | 受信写真/動画のdouble-click起動とdraft画像サムネイル、contact/媒体の正しい対応。追加ユーザー報告の起動SIGSEGV（再読込したcaptionなし写真）も修正 | cleared | p010受信 source 89d487814、p011 |
| media-uat-i02 | ws127-p013 | Files itemのdouble-clickはreleaseまで待ち、drag時はopenしない、通常double-click維持 | cleared | 現main DnD source |
| media-uat-i03 | ws157-p008 | folder選択、background import/refresh、JPEG実fixtureの調査・修正 | cleared | p006 source、ユーザー添付4080×3072 JPEG |
| media-uat-i04 | ws157-p009 | 今回全変更C全文/manual/format、短い意味のあるhost試験、named build warning0、main統合 | cleared | i01、i02、i03 |

Graph: i01 / i02 / i03 → i04。同期公開なし・QEMU/aggregate make check/共有build/toolchain変更なし。旧Queueは[履歴](history/media-receive-finished-20261010.md)。WS197送信/HFP/PBAP/p009、WS157ベータ3 scope等は保持。画像fixtureはprivate attachmentとして読み、repoへ画像/撮影地等を記録しない。

## 2026-10-10 最終結果 / Past Log

全4attempt cleared。source `e654733f1eb6e1296eacf34b5dff6002a5dd17e1` をmainへ統合、HEADとclean状態を読み返した。[検証記録](../ws157/tests/media-uat-verification-20261010.md)。Phone起動SIGSEGVは実機再現/修正版exit0、提供JPEGはGPU16MiB制限を避けるCPU描画で実機exit0、mediastorageは原寸metadata/import成功。短いhost probe4本PASS、named build/Linux lib warning0、変更source全文規約の新規finding0（既存30件を保持）。

実機5file更新済み（Photos最終CRC3049254075）、新しい添付viewer/thumbnail・Files drag・folder chooser・背景importのGUI UATは次回アプリ起動でユーザー確認。WS197のMMS送信/HFP/PBAP/p009、WS127 p007、WS157ベータ3の全体受入は残る。既存JPEG2件のサムネイルEINVALは原因/旧版同条件未確認であり、この提供JPEGの修正と混同しない。共有master/Queue/cacheへの投影はQ1、GitHub未公開。次Queueは開始しない。
