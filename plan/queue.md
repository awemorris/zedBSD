<!-- awesome-plan-current:start -->

## 現在状態 — 2026-09-12

Active Queue: none
fg006: completed

2026-09-12、ユーザーがWS003 p022・p023・p024の完了を報告し、MarkdownとGitHubの更新を指示した。3 Phaseをcleared（disposition: normal）として受け入れ、fg006を完了とする。WS003はインストーラ等の残件があるためincompleteを維持する。新しいQueueは作成せず、保留中の実行は再開しない。

対象: [p022](https://github.com/awemorris/zedBSD/issues/88)、[p023](https://github.com/awemorris/zedBSD/issues/89)、[p024](https://github.com/awemorris/zedBSD/issues/90)。

根拠は今回のユーザー完了報告。既存Markdownの自動検証・旧試行の結果は履歴として保持する。今回エージェントがbuild・QEMU・実機検証を再実施したものではなく、新しいartifact hashやroot/init/login到達点は報告されていないため追加しない。旧試行のunclearedや未実施項目を過去に遡ってPASSへ変更しない。

<!-- awesome-plan-current:end -->

# Queue q303: i386 callback clarity and shared x86 verification

Date: 2026-09-10
Status: finished
Authorization: explicit HAL refactor and standing execution
Timebox: 60 active minutes
Previous: [q302](history/queue-q302.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p038](ws025/phase038/phase.md) | uncleared | i386 normalized callback style, builds and shared x86 runtime |

Stopped at user request. Three builds PASS; amd64 runtime interrupted and
PC/AT runtime not started. Resume only after user resumes execution.

## Awesome Plan運用開始時点

Active Queue: none。q303はfinished、p038試行はuncleared。過去の自走・HAL承認を再開許可にしない。
Upcoming Work Outlook: fg004 / WS003の4機種インストーラbring-upを計画中。PC98 QEMU /sbin・menuconfig、LBA0後停止、LX6 boot媒体識別の切り分けが候補。方式・媒体・観測入力を具体化してから有限Queueを選ぶ。実行承認・順序・時間枠は未設定。旧Priorityリストとq303は再開しない。
新しい実行指示を受けたら、既存Phaseと現行コードを確認して有限Queueを提示する。

## 2026-09-11 後続の計画判断

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active Queueと新しいPriority/Focusはない。

q303の試行結果unclearedは当時の結果として保持。p038の現在のclearedは今回のユーザー判断であり、q303の再実行・合格ではない。

## 2026-09-11 インストーラ実機bring-up計画

[ws003](ws003/ws.md)でfg004を管理。既存p024/p018/p019と新p026〜p032に具体化。PC98/LX6の修復とSV7/Latitudeの準備は独立する範囲を持つ。新しいactive Queueなし。

## 2026-09-11 ネットワーク改善の追加Outlook

fg005 / [ws005](ws005/ws.md) p013〜p017を計画。共通enable/ready/通知契約の確認後、有線管理→起動待機/通知→統合確認が依存上の候補。fg004のインストーラ計画を保持し、両Goal間の順位は未指定。active Queue・実行時間枠・開発承認なし。

## 2026-09-11 fg006: PC-9821V13での起動改善

ユーザーがCurrent Focused Goalsへの追加と、WS003 p022/p023/p024の具体的実行Phase化を明示指示した。fg004（4機種インストーラ）・fg005（ネットワーク）を保持する。

| 順序 | 実行Phase | 成果 |
| --- | --- | --- |
| 1 | [ws003-p022](ws003/phase022/phase.md) | 現行IPLのstack・BIOS read契約、artifact対応、診断の前提 |
| 2 | [ws003-p023](ws003/phase023/phase.md) | LBA0実行後の実機停止境界と原因を絞る観測 |
| 3 | [ws003-p024](ws003/phase024/phase.md) | 根拠に対応した修正と通常imageでのV13起動確認 |

依存: p022 → p023 → p024。p022/p023は旧履歴専用の扱いを解除し、現行の役割・受け入れを上記と各Phaseに更新する。過去の試行・証拠は保持する。3 Phaseはunclearedのまま次の試行を待ち、今回in-progressやclearedにしない。
この順序は選択した3 Phaseの依存順であり、削除済みの全WS Priorityリストを復活させない。今回の依頼は計画更新。新しいactive Queue・実行時間枠は未設定。

この3 Phaseを次の具体的な実行対象としてOutlookに保持。q303のfinished/stopped履歴は変更しない。
