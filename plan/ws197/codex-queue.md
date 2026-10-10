# WS197 Codex 承認済み実行記録

Status: finished
Owner: Codex / codex/ws197-pbap-sms
Approval: current user、2026-10-10、WS197 p005 完了と新規受信SMS不具合の修正。Pc1〜Pc6 推奨どおり。
Scope: [p005](phase005/phase.md) 第3.1版の i06/i07 残り、最終全文規約・host・named build と SMS 受信経路診断修正。調査は実機の読み取り＋同経路の有界 host 回帰。実機PBAP UATはp008。

| 項目 | 状態 | 依存 |
| --- | --- | --- |
| p005 i06/i07・最終検証 | cleared | main に統合済み i01〜i05・既存store |
| 新規SMS/MMSテキスト受信表示 | cleared | 実機SSH成功、MAPの受信通知経路 |

共有 Queue の ID は Q1 が管理。独自の q 番号を割り当てない。master.md は変更しない。終了時に WS/Phase に結果と残りを記録し、この範囲だけ main へ統合する（既存ユーザー承認）。

## 2026-10-10 新規受信の診断とMMSテキスト承認

実機 beta2+ge8adcdf の17:41 JSTのMNS通知は `type=1 message-type=3 handle-present=1`（NewMessage / MMS）。通知自体は到達し、MAPのSMS限定条件とlisting filterで除外されていた。本文・番号・handleは記録しない。ユーザー回答「MMSのテキスト受信も含める」により、SMS受信修正へMMSのテキスト本文の抽出・履歴取り込み・通知・保存・表示を追加する。添付画像・動画は対象外、MIME生データを本文として表示しない。MIME parserは独自Zlib実装、RFC2045/2046のtransfer encodingとmultipartを参照。共有master/queueへの投影はQ1に保留。

## 終了（2026-10-10）

p005 i06/i07はhost・named build warning0・最終変更source全文規約でcleared。新規受信はMMS除外と末尾CRLFの2点を修正、ユーザーが「ゴミは消え、日本語も受信できました」と実機確認してcleared。結果は[p005](phase005/phase.md)・[p004](phase004/phase.md)・[WS197](ws.md)へ記録。WS197全体はincomplete、次Phaseの自動開始はしない。

WS199はユーザー受け入れでcompleted/local closed、[完了証拠](../ws199/ws.md)に記録。remote issueが存在せずGitHub closeは行わない。Q1への未反映事項: master/共有Queue/Past Logの結果投影、WS197旧p003/current headerの履歴整理、WS199 completed投影とPhase/固有testの整理。共有記録と外部sessionの削除境界を守り、master/Queueは変更しない。既存方針によりGitHub公開は保留。
