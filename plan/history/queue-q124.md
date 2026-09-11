# Queue q124: PC-98 と複数Wi-Fiの回帰修正

Date: 2026-09-09 JST
Status: finished
Authorization: ユーザーが Phase 作成・queue 登録・実行を明示指示。
Timebox: 従来同様 90 active minutes ごとに証拠と進捗を確認する。
Previous: [q123](queue-q123.md), finished / p031 uncleared。

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p032](../ws025/phase032/phase.md) | completed | 最優先の PC-98 QEMU 起動回復 |
| 2 | [ws025-p033](../ws025/phase033/phase.md) | completed | AX211 VFIO 自動接続・DHCP・復旧。p032 後に実行。独立した読取りの環境確認は先行可 |
| 3 | [ws025-p034](../ws025/phase034/phase.md) | completed | 直接 wifi up/down/connect 反復後の無限エラーを再現・原因修正。最新ユーザー依頼で追加実行承認済み。p033 の実機試験後に実行 |
| 4 | [ws025-p035](../ws025/phase035/phase.md) | completed | AX211＋RTL8822BU の同時接続とスキャン待ち遅延。最新追加試験依頼で実行承認済み。p034 反復試験後に実行 |

p027–p030 は対象外。現行のユーザー変更を保持する。
commit・aggregate make check・.internal 参照なし。make -j16、ビルド・テスト・runtime は直列。

## 最終結果

q124はp032–p035すべてcompleted。p035では実AX211＋RTL8822BU同時構成の
両操作順序・再enable・scan取消しからL2/DHCP/pingが成立。
enable→鍵登録のOFFER/bound観測は約40秒から約6.5秒に短縮した。
別順序ではAX211側もwinnerとして接続を確認。36 host stories、common/PCI検証、
3 architecture build、AX211単独12反復の回帰を完了しホストを復元した。
[詳細と残存firmware問題](../ws025/phase035/results.md)。
p031のunclearedと、採用条件未成立のp027–p030はこの結果では変更しない。
