# Codex MMS受信の承認済みQueue

Status: active
Owner: 本Codexセッション、codex/ws197-media-receive、base c43a01797。共有master/Queue/cacheはQ1のみ。

承認: 2026-10-10ユーザー「受信の通知は届いたのですが、タイムラインにはファイル名しか表示されず、写真を保存もできていなかったです。sshで試してください。ホストキーは削除して更新してOKです。」以前の写真・動画の送受信承認とmain統合承認は保持。今回は受信の取得/FD中継/Media保存/Phone表示・再openと最終規約/build/SSH確認を有限scopeとして実行する。送信4機能全体のp010完了とは分ける。既存SMS/MMS本文を維持し、本文・番号をログに保存しない。

| Attempt | Phase | Scope | Status | 依存 |
| --- | --- | --- | --- | --- |
| media-rx-i01 | ws197-p010（部分） | MAP添付取得・MIME・FD中継・mediastorage保存・timeline写真/動画項目 | cleared (host/build scope) | ws157-p006/p007、ws197-p011のmain実source確認済み |
| media-rx-check-i01 | ws197-p010（部分） | 全文規約、host受信回帰、named build、SSH実機確認、main統合 | in-progress (SSH回復・受信UAT待ち) | media-rx-i01 |

Graph: media-rx-i01 → media-rx-check-i01。以前の完了Queueは[履歴](history/media-foundation-20261010.md)。p010の送信未完義務は保持。QEMU/aggregate make check/toolchain変更/共有plan変更はしない。

## 2026-10-10 更新の承認と検証checkpoint

ユーザー回答「更新・再起動してよい」を4ファイル交換とBluetooth/desktop restartの承認として保存。host受信・再open・実decoder、zedBSD4target、Linux共通interface/compositor、OS境界、最終変更source全文規約は通過。[証拠](tests/media-receive-verification-20261010.md)。実機への更新と新desktop/Phone起動までは確認したが、Bluetooth再接続が停止し、再restart後のSSHもtimeout。画面応答/必要なら実機restartをユーザーに依頼中。新写真の実機受信・最終Phone差し替え・main統合記録をcheck項目の残りとして保持し、Queue全体はactive。
