# Codex メディア管理の承認済み実行記録

Status: finished
Owner: 本Codexセッション、codex/ws197-media-library、base main 9dfebc99b。P1/P2とは別tree。共有master/Queue/cacheはQ1の更新待ち。実行上限は下記の有限機能と検証。

## 承認

2026-10-10 ユーザー: メディア取得・追加をcompositorの機能へ、DB操作と保存はCLIへ。メタファイル＋実ファイルfolder。CLIの変更通知をcompositorが監視appへ中継。Photosの取得/追加もこのAPI経由。Phoneは＋からメディアを選び仮添付、写真/動画/textのDnDを受け付け、textは入力欄へ追加、送信操作まで保留。最後の限定によりMMS送受信接続は基盤後に行う。確認質問への回答「ドラッグ＆ドロップも今回含める」。main統合は既存の承認が継続。実際の外部message送信はしない。

| Item | Phase | 範囲・判定 | Status | 依存 |
| --- | --- | --- | --- | --- |
| media-cli | ws157-p006 | Media JSON/Files形式のCLI取得/追加/更新、画像・動画、同時更新/復旧、通知 | cleared | 既存p004 source確認済み |
| media-api | ws157-p006 | backend CLI実行、compositor API/監視、libkeiland API、Photos利用 | cleared | media-cli |
| phone-select | ws197-p011 | ＋でlibraryから選択・仮添付、image/video/text DnD、取消・未送信 | cleared | media-api |
| conformance | ws157-p007 / ws197-p011 | 全変更のC全文規約・意味のあるhost回帰・named build warning0・main統合 | cleared | 上の3件 |

Graph: media-cli → media-api → phone-select → conformance。MMS codec/Get/Pushの途中差分は別treews197-mediaに保管、未統合でuncleared。今回scopeではMMS転送の完成を報告しない。master/共有Queue/registryへの投影はQ1に保留。リリースimage、toolchain更新、QEMU全面試験、実機desktop再起動は範囲に含めない。

## 2026-10-10 パス経由の受け渡し

ユーザー: 「CLIは写真取得に関して、写真の一覧のメタデータを返し、その中にファイルのパスが入っていればいいです。写真本体はファイル経由でやりとりしましょう。写真の追加に関しては、このパスの写真を追加して、というリストを渡しましょう。」取得はバージョン付きTSVのID・絶対パス・hash・サイズ・日時・寸法・favorite・rotation・元の名前。追加は標準入力の改行区切りパスリスト。画像/動画本体はCLI/拡張IPCへ載せない。既存Photosの保存形式を維持する。PNGのDnDだけは既存の画像drop APIで受信後、Phoneの一時ファイルへ保存して仮添付する。

以前のSMS/PBAP完了Queueは[凍結記録](history/pbap-sms-20261010.md)に保存。MMS先行試作Queueは別tree `codex/ws197-mms-media` の `plan/ws197/codex-queue.md` とp010に保存し、未統合/unclearedを維持。

## 2026-10-10 通信境界の明確化

ユーザーの指定: CLIは `/bin/mediastorage`。CLI/Keiland appsのcompositor通信はlibkeilandのWayland拡張のみで、直接UNIXソケットは禁止。OS処理は静的libkeiland-backendへ置き、zedBSD backendからposix_spawnでCLIを起動し、stdin/stdoutのパイプでメタデータ・パスリストを交換する。通常の実装選択の明確化として現Queueへ反映。共有Guardrail/concise/Queueの投影はQ1に保留。backendは両方向を非同期pumpし、Wayland FDへ渡す完了metadataだけanonymous spoolへ保持する。

## 2026-10-10 保存形式の変更承認

ユーザー指定を優先し、従来の `~/Pictures/Library` 月別TSV保存を現scopeで置き換える。`~/Pictures/Media/metadata.db` はversion付きJSON、原本copyは `Media/Files/YYYY/MM/dd/名前`。JPEG EXIF撮影日時がなければPNG/JPEG/動画等は取り込み日で整理する（元ファイルmtimeは使わない）。日付・バイト数・画像寸法・hash・原名・favorite・rotation・albumを保持し、未知のJSON fieldを更新時にも保存することで撮影地等へ拡張可能にする。既存Libraryの自動移動/削除はしない。旧ファイルはそのまま、必要な原本はmediastorage addで再取り込みできる。p006設計・p007検証・p011の選択元へ同じ承認を反映。以前のcleared履歴は旧形式の履歴として維持する。

## 2026-10-10 実行終了

4 itemはすべてcleared（本scopeのみ）。source commit `874e12d3b`、main fast-forward/read-back確認済み。[最終検証](../ws157/tests/verification-20261010.md)・[CLI利用法/実機手順](../ws157/tests/mediastorage-usage.md)。zedBSD 5 named targetsとLinux all warning0、ASan/UBSan host回帰と境界PASS。WS157/WS197はincomplete。実機GUIとMMS転送は未実施。共有master/Queue/Guardrail/cache投影はQ1に保留。local-onlyでremote sync義務なし。

Upcoming Work Outlook（未選択・自動開始なし）: Media/Phoneの実機UI手順、p010のMMS写真/動画Get/Push接続（別treeの試作あり）、WS197 HFP/PBAP UAT/最終conformance。scope/依存を確認して次Queueへ選択する。
