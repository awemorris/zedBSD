<!-- awesome-plan project=zedbsd record=ws157-p006 -->

# ws157-p006: メディア管理CLIとcompositor API・Photos移行

Parent: [WS](../ws.md)
Status: cleared
Phase disposition: normal
Queue: [Codex実行記録](../../ws197/codex-queue.md)

2026-10-10ユーザーのメディア管理先行指示とDnD確認を承認源とする。現在の保存仕様は~/Pictures/Media/metadata.db（拡張可能JSON）とFiles/YYYY/MM/dd。SHA256 dedupを維持。動画を同じ管理へ追加。CLIだけが保存形式を読み書きし、backendがCLIを実行、compositorが取得・追加・監視・変更通知を仲介。Photosは起動中/再起動後に更新を受け取り、追加/取得/metadata変更はAPI経由。Phoneは＋でlibraryを選び、image/video/text DnDを受ける。メディアは未送信の仮添付、textは入力欄へ、外部送信はユーザー操作のみ。MMS接続は後続p010へ残す。

Standards: [Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[automation](../../standards/automation.md)。例外なし。共有master/Queue/cacheは書かずQ1へ投影保留。compositor/clientはOS固有DB操作を持たない。CLIの通知不達でも保存結果を失わず、監視appには更新を中継、startupはCLIの最新DB。

Criteria: CLI画像/動画import、dedup/再読込/metadata/CLI通知、backend非同期CLI実行と失敗/FD所有、Wayland取得/追加/watch、Photos反映を意味のあるhost回帰で確認。最終source全文規約、diff-check、mediastorage/wayland/libkeiland/photos/phone named build warning0、共通Linuxbuild。実機UATは未実施と明記し手順提示。

Resume: 今回の有限実装/host/build scopeはcleared。実機GUI UATは記載手順から確認し、新規source変更時は該当全文規約を再検証する。

## 2026-10-10 パス経由の受け渡し

ユーザー: 「CLIは写真取得に関して、写真の一覧のメタデータを返し、その中にファイルのパスが入っていればいいです。写真本体はファイル経由でやりとりしましょう。写真の追加に関しては、このパスの写真を追加して、というリストを渡しましょう。」取得はバージョン付きTSVのID・絶対パス・hash・サイズ・日時・寸法・favorite・rotation・元の名前。追加は標準入力の改行区切りパスリスト。画像/動画本体はCLI/拡張IPCへ載せない。保存形式は後述のユーザー指定によりJSONへ置き換える。PNGのDnDだけは既存の画像drop APIで受信後、Phoneの一時ファイルへ保存して仮添付する。

2026-10-10構造の明確化: p006はCLI/compositor/API/Photos、PhoneのUI/DnDの判定は[ws197-p011](../../ws197/phase011/phase.md)が所有する。p011の前提はp006のAPI出力（named buildとhost-media-wireで確認済み）、最終の全変更規約はp007とp011でそれぞれ自身のsourceに適用する。有限承認scopeは変更しない。

## 2026-10-10 通信境界の明確化

ユーザーの指定: CLIは `/bin/mediastorage`。CLI/Keiland appsのcompositor通信はlibkeilandのWayland拡張のみで、直接UNIXソケットは禁止。OS処理は静的libkeiland-backendへ置き、zedBSD backendからposix_spawnでCLIを起動し、stdin/stdoutのパイプでメタデータ・パスリストを交換する。通常の実装選択の明確化として現Queueへ反映。共有Guardrail/concise/Queueの投影はQ1に保留。backendは両方向を非同期pumpし、Wayland FDへ渡す完了metadataだけanonymous spoolへ保持する。

## 2026-10-10 保存形式の変更承認

ユーザー指定を優先し、従来の `~/Pictures/Library` 月別TSV保存を現scopeで置き換える。`~/Pictures/Media/metadata.db` はversion付きJSON、原本copyは `Media/Files/YYYY/MM/dd/名前`。JPEG EXIF撮影日時がなければPNG/JPEG/動画等は取り込み日で整理する（元ファイルmtimeは使わない）。日付・バイト数・画像寸法・hash・原名・favorite・rotation・albumを保持し、未知のJSON fieldを更新時にも保存することで撮影地等へ拡張可能にする。既存Libraryの自動移動/削除はしない。旧ファイルはそのまま、必要な原本はmediastorage addで再取り込みできる。p006設計・p007検証・p011の選択元へ同じ承認を反映。以前のcleared履歴は旧形式の履歴として維持する。

## 2026-10-10 最終検証

[コマンド・source範囲・結果・限界](../tests/verification-20261010.md)。実装/host/build criteriaはPASS、例外なし。main統合を行い、統合後にscoped clearanceを記録する。実機GUI UATとMMS転送は成功扱いにしない。

## 2026-10-10 outcome / main統合

実装source commit `874e12d3b` をmainへfast-forward統合しread-backで確認。今回承認されたhost/buildまでのcriteriaを満たし **cleared**。実機UATは未実施、MMS添付転送はws197-p010へ残る。WS全体のcompletedとは扱わない。local-onlyの記録、共有master/Queue/Guardrail/cacheへの投影はQ1に保留。今回scopeのsourceに未コミット変更はない。
