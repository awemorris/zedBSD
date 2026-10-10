# Codex Bluetooth復旧 Queue

Cycle: bluetooth-recovery-20261010
Status: finished
Owner: 本Codexセッション、codex/ws197-media-uat、base 1b07745e4。shared master/Queue/cacheはQ1。
Approval: 2026-10-10ユーザーのBluetooth無応答報告とdaemon restart/device reopen/resetの必要性の指示。追加指示: daemon無応答時に自動再起動する仕組みも実装する。root親で進捗を監視し既存service failure restartへ接続、スマホ側の接続削除は現状維持。以前のSSH/実機更新/main統合承認を保持。

| Attempt | Phase | Scope / Criteria | Status | Dependency |
| --- | --- | --- | --- | --- |
| bt-recovery-i01 | [ws197-p013](phase013/phase.md) | 実機診断/既存daemon restart、bounded CLI/CHECK/REOPEN/RESET、HCI wait補完、親監視→自動restart、短いhost/全文規約/named build warning0/main統合 | cleared | main 1b07745e4、WS143既存UAPI/privsep |

Graph: 検証済み既存出力→i01。有限scopeはphase013を参照。直前の[媒体UAT Queue](history/media-uat-finished-20261010.md)はfinished履歴としてbyte一致を確認し保存。desktop/Bluetoothの全体再設計、bond削除、kernel/HAL/UAPI変更、MMS送信/HFP/PBAP全体、shared boardsは含めない。


## Outcome / 2026-10-10

bt-recovery-i01 cleared、source 7fa0a0e9cはmain read-back済み。build warning0、6source全文規約/host probe PASS、実機daemon自動再起動/CLI5秒timeout/REOPEN/RESET各成功。[証拠](tests/bluetooth-recovery-verification-20261010.md)。既存failure restart上限5回と元hang根因未確定は保持。スマホ接続削除は現状維持。WS197はincomplete（HFP/PBAP UAT/MMS送信/p009は未完）。共有master/Queue/cache/remote投影はQ1。push無し。

Outlook: ユーザーの次の実機操作でPhone/媒体UI UAT、スマホ再pairはユーザーの都合で実施。HFP・PBAP実機・MMS送信は既存WS計画を参照。このforecastは新実行許可ではない。
