<!-- awesome-plan project=zedbsd record=ws157-p007 -->

# ws157-p007: メディア管理の全文規約と最終検証

Parent: [WS](../ws.md)
Status: in-progress
Phase disposition: normal
Queue: [Codex実行記録](../../ws197/codex-queue.md)

2026-10-10ユーザーのメディア管理先行指示とDnD確認を承認源とする。現在の保存仕様は~/Pictures/Media/metadata.db（拡張可能JSON）とFiles/YYYY/MM/dd。SHA256 dedupを維持。動画を同じ管理へ追加。CLIだけが保存形式を読み書きし、backendがCLIを実行、compositorが取得・追加・監視・変更通知を仲介。Photosは起動中/再起動後に更新を受け取り、追加/取得/metadata変更はAPI経由。Phoneは＋でlibraryを選び、image/video/text DnDを受ける。メディアは未送信の仮添付、textは入力欄へ、外部送信はユーザー操作のみ。MMS接続は後続p010へ残す。

Standards: [Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[automation](../../standards/automation.md)。例外なし。共有master/Queue/cacheは書かずQ1へ投影保留。compositor/clientはOS固有DB操作を持たない。CLIの通知不達でも保存結果を失わず、監視appには更新を中継、startupはCLIの最新DB。

Criteria: CLI画像/動画import、dedup/再読込/metadata/CLI通知、backend非同期CLI実行と失敗/FD所有、Wayland取得/追加/watch、Photos反映、Phone選択/DnD仮添付を意味のあるhost回帰で確認。最終source全文規約、diff-check、mediastorage/wayland/libkeiland/photos/phone named build warning0、共通Linuxbuild。実機UATは未実施と明記し手順提示。

Resume: 最終sourceの実装・host検証・named buildは完了。承認済みmain統合とoutcome記録を行う。

## 2026-10-10 通信境界の明確化

ユーザーの指定: CLIは `/bin/mediastorage`。CLI/Keiland appsのcompositor通信はlibkeilandのWayland拡張のみで、直接UNIXソケットは禁止。OS処理は静的libkeiland-backendへ置き、zedBSD backendからposix_spawnでCLIを起動し、stdin/stdoutのパイプでメタデータ・パスリストを交換する。通常の実装選択の明確化として現Queueへ反映。共有Guardrail/concise/Queueの投影はQ1に保留。backendは両方向を非同期pumpし、Wayland FDへ渡す完了metadataだけanonymous spoolへ保持する。

## 2026-10-10 保存形式の変更承認

ユーザー指定を優先し、従来の `~/Pictures/Library` 月別TSV保存を現scopeで置き換える。`~/Pictures/Media/metadata.db` はversion付きJSON、原本copyは `Media/Files/YYYY/MM/dd/名前`。JPEG EXIF撮影日時がなければPNG/JPEG/動画等は取り込み日で整理する（元ファイルmtimeは使わない）。日付・バイト数・画像寸法・hash・原名・favorite・rotation・albumを保持し、未知のJSON fieldを更新時にも保存することで撮影地等へ拡張可能にする。既存Libraryの自動移動/削除はしない。旧ファイルはそのまま、必要な原本はmediastorage addで再取り込みできる。p006設計・p007検証・p011の選択元へ同じ承認を反映。以前のcleared履歴は旧形式の履歴として維持する。

## 2026-10-10 最終検証

[コマンド・source範囲・結果・限界](../tests/verification-20261010.md)。実装/host/build criteriaはPASS、例外なし。main統合を行い、統合後にscoped clearanceを記録する。実機GUI UATとMMS転送は成功扱いにしない。
