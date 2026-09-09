# Queue q126: WS006 evdev移行の完結

Date: 2026-09-09
Status: finished
Authorization: Priority全件のPhase設計・Queue化・自走をユーザー明示承認済み。
Timebox: 90 active minutesごとに証拠・進捗をレビュー。
Previous: [q125](queue-q125.md) finished / WS025-p027〜p030 uncleared。

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws006-p009](ws006-input/phase009-consumer-legacy-removal/phase.md) | uncleared | 不使用console event UAPIと専用queue/ownerを削除し、TTY broker・evdev consumer・通常起動を検証する。 |

現ソース調査で旧poll/read helperのkernel consumerは既に不在。HAL→input subscriber→TTYの経路は保持する。
現行ソースに合わせたhost fixture更新、UAPI unsupported gate、amd64/PCAT/PC98 buildとローカルQEMUを直列実行。
実機条件または外部consumer確認が未完なら明記しunclearedとしてWS022へ進む。
commit・aggregate make check・.internal参照なし。

結果: [p009 results](ws006-input/phase009-consumer-legacy-removal/results.md)。実装・通常ビルド・主要host/QEMUはPASS、paired USB root列挙とXzed GUI確認を残す。
