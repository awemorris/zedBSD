# Queue q125: WS025 optional I/O phases

Date: 2026-09-09
Status: finished
Authorization: ユーザーがPriority全件のPhase設計・Queue化・自走と、未クリア時の他WSへの移行を明示指示。追加確認不要。
Timebox: 90 active minutesごとの進捗・証拠レビュー。
Previous: [q124](queue-q124.md) finished。

現ソース・既存証拠・機器条件の照合を先行。根拠のない性能最適化は実装せず、各Pへ具体的設計と再開条件を記録。

| Order | Phase | Status | Result |
| --- | --- | --- | --- |
| 1 | [ws025-p027](ws025-io-memory-cache/phase027-nvme-queue-depth/phase.md) | uncleared | 対象NVMeでdepth律速を示す測定が未成立。テスト機SSHは接続timeout。既存multi-slotを新実装と数えて完了にはできない。 |
| 2 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | direct user I/Oを正当化する対象workloadのcopy律速測定がなく、既存pinだけではalias/COW整合を満たせない。p024のコピー削減結果をdirect I/OのCPU改善と読み替えない。 |
| 3 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | 対象UAS機器とdescriptorが未確認。現Phaseの実機先行条件を満たさず、架空descriptorに合わせたdriverは実装しない。 |
| 4 | [ws025-p030](ws025-io-memory-cache/phase030-imod-measurement/phase.md) | uncleared | 実機比較の実行環境と複合topologyを取得できず、既定値選択に必要なデータがない。 |

実機read-only inventoryはssh接続timeout（ConnectTimeout=8）。ローカルlsusbは未導入で機器証拠に使っていない。
p027〜p030は完了ではない。WS025を未完了のまま保持してWS006へ進む。commit・make check・.internal参照なし。
