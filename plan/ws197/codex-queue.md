# Codex MMS受信の承認済みQueue

Status: active
Owner: 本Codexセッション、codex/ws197-media-receive、base c43a01797。共有master/Queue/cacheはQ1のみ。

承認: 2026-10-10ユーザー「受信の通知は届いたのですが、タイムラインにはファイル名しか表示されず、写真を保存もできていなかったです。sshで試してください。ホストキーは削除して更新してOKです。」以前の写真・動画の送受信承認とmain統合承認は保持。今回は受信の取得/FD中継/Media保存/Phone表示・再openと最終規約/build/SSH確認を有限scopeとして実行する。送信4機能全体のp010完了とは分ける。既存SMS/MMS本文を維持し、本文・番号をログに保存しない。

| Attempt | Phase | Scope | Status | 依存 |
| --- | --- | --- | --- | --- |
| media-rx-i01 | ws197-p010（部分） | MAP添付取得・MIME・FD中継・mediastorage保存・timeline写真/動画項目 | cleared (host/build scope) | ws157-p006/p007、ws197-p011のmain実source確認済み |
| media-rx-i02 | ws197-p010（部分） | leaf/WAP型に明示boundaryを付けるスマホMMSの本文・添付解析を修正 | cleared (host/build/SSH履歴保存 scope) | media-rx-i01、実機50feed3の再現 |
| media-rx-check-i01 | ws197-p010（部分） | 全文規約、host受信回帰、named build、SSH実機確認、main統合 | in-progress (新規受信UAT待ち) | media-rx-i01、media-rx-i02 |

Graph: media-rx-i01 → media-rx-i02 → media-rx-check-i01。以前の完了Queueは[履歴](history/media-foundation-20261010.md)。p010の送信未完義務は保持。QEMU/aggregate make check/toolchain変更/共有plan変更はしない。

## 2026-10-10 更新の承認と検証checkpoint

ユーザー回答「更新・再起動してよい」を4ファイル交換とBluetooth/desktop restartの承認として保存。host受信・再open・実decoder、zedBSD4target、Linux共通interface/compositor、OS境界、最終変更source全文規約は通過。[証拠](tests/media-receive-verification-20261010.md)。実機への更新と新desktop/Phone起動までは確認したが、Bluetooth再接続が停止し、再restart後のSSHもtimeout。画面応答/必要なら実機restartをユーザーに依頼中。新写真の実機受信・最終Phone差し替えをcheck項目の残りとして保持し、Queue全体はactive。

main統合: source/evidence `035d1d25b` をc43a01797からfast-forward済み、source同一/cleanを確認。check項目はSSH/UAT待ちでin-progressを維持。

## 2026-10-10 実機受信の回帰修正 i02

ユーザー「テキストは受信できましたが、なんとMIMEヘッダも見えてしまってます。画像のMMSは、今度は通知が来ませんでした。SSHで見てみてください。」を受信部分修正/SSH確認の継続指示として保存。media-rx-i01のhost履歴は保持するが実機acceptanceは不足。media-rx-i02をin-progressとして追加し、check項目はこの出力に依存する。範囲: 明示boundaryのあるleaf/WAP MIMEをpartとして解析し、本文/原本byteを直し、限定host/build/実機更新/再同期で確認。外部送信なし、共有plan変更なし。

i02結果: codec 3種のphone-shaped MIME、実Phone→compositor→CLI→原本/decode/再openのhost確認、3実行ファイルbuild warning0、変更3Cのstyle-check新規0を通過。実機3ファイルをバックアップして更新し、既存承認のBluetooth/desktop restart後、MAP/PBAP readyとPhone再同期を確認。Media/Files 0→2件、Media-Photo付きmessage 5件、受信media error0。部分修正scopeをcleared、新規テキスト/写真MMSの通知・画面表示はユーザーへ確認依頼中。詳細は[検証記録](tests/media-receive-verification-20261010.md)。
