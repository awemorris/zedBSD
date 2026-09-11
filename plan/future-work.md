# Future Work

現在のPriority・実行Queueから外した将来項目。着手対象として選び直した時点で計画を更新する。
WS/Phase IDと完了済みの実績は消去・再利用しない。

| ID | 項目 | 保持する内容・再開点 |
| --- | --- | --- |
| F-001 | bridge | [旧WS011-p004](ws011/phase004/phase.md)からbridgeだけを移管。L2転送、member所有権、設定・永続化を独立して設計する。VLANは含めない。 |
| F-002 | [WS013：CPAR](ws013/ws.md) | Runtimeの名前空間・隔離、CLI/build、サービスコンテナを将来へ。完了済みp002〜p006のブート設定基盤は現行機能として維持する。 |
| F-003 | [WS015：μITRONリアルタイム領域](ws015/ws.md) | 互換プロファイル、RT/POSIX境界、常駐実行、通信・障害・時間保証の設計を将来へ移管。 |

VLANは2026-09-09ユーザー指示によりキャンセル。Future Listにも残さない。
旧MB-010のVLAN/bridge一括保留は終了し、bridgeはF-001として扱う。

