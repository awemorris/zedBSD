<!-- awesome-plan-current:start -->

## 現在状態 — 2026-09-12

Active Queue: none
fg006: completed

2026-09-12、ユーザーがWS003 p022・p023・p024の完了を報告し、MarkdownとGitHubの更新を指示した。3 Phaseをcleared（disposition: normal）として受け入れ、fg006を完了とする。WS003はインストーラ等の残件があるためincompleteを維持する。新しいQueueは作成せず、保留中の実行は再開しない。

対象: [p022](https://github.com/awemorris/zedBSD/issues/88)、[p023](https://github.com/awemorris/zedBSD/issues/89)、[p024](https://github.com/awemorris/zedBSD/issues/90)。

根拠は今回のユーザー完了報告。既存Markdownの自動検証・旧試行の結果は履歴として保持する。今回エージェントがbuild・QEMU・実機検証を再実施したものではなく、新しいartifact hashやroot/init/login到達点は報告されていないため追加しない。旧試行のunclearedや未実施項目を過去に遡ってPASSへ変更しない。

<!-- awesome-plan-current:end -->

# Past Log

## 2026-09-11 fg006: PC-9821V13での起動改善

ユーザーがCurrent Focused Goalsへの追加と、WS003 p022/p023/p024の具体的実行Phase化を明示指示した。fg004（4機種インストーラ）・fg005（ネットワーク）を保持する。

| 順序 | 実行Phase | 成果 |
| --- | --- | --- |
| 1 | [ws003-p022](../ws003/phase022/phase.md) | 現行IPLのstack・BIOS read契約、artifact対応、診断の前提 |
| 2 | [ws003-p023](../ws003/phase023/phase.md) | LBA0実行後の実機停止境界と原因を絞る観測 |
| 3 | [ws003-p024](../ws003/phase024/phase.md) | 根拠に対応した修正と通常imageでのV13起動確認 |

依存: p022 → p023 → p024。p022/p023は旧履歴専用の扱いを解除し、現行の役割・受け入れを上記と各Phaseに更新する。過去の試行・証拠は保持する。3 Phaseはunclearedのまま次の試行を待ち、今回in-progressやclearedにしない。
この順序は選択した3 Phaseの依存順であり、削除済みの全WS Priorityリストを復活させない。今回の依頼は計画更新。新しいactive Queue・実行時間枠は未設定。

新規source変更・build・実機操作・GitHub公開は未実施。

## 2026-09-11 ネットワーク改善1〜3（計画のみ）

ユーザー指定をfg005 / [ws005](../ws005/ws.md) p013〜p017に整理。net lan disableは無効化。network-enableは有線かWi-FiのどちらかのIP取得で待機終了、既定30秒・設定可能、timeoutでも通常起動とdaemon接続処理を継続することをユーザーが確認。状態通知のsocket/fileは設計選択として保持。現行net wifi enableはバックグラウンド接続開始であることを静的確認した。

既存完了Phase、インストーラfg004、旧Priority削除を維持。新規実装・build・ネットワーク変更・公開なし。GitHub更新は未承認のまま保留。

## 2026-09-11 インストーラ実機bring-up計画（実行なし）

ユーザーがPC98 / Latitude 5320 / SV7 / LX6でのインストールを指定。[ws003](../ws003/ws.md)のfg004として、既存Phaseを再利用しp026〜p032を計画、BUG-013を既存IDで詳細化しBUG-023〜025を追加。PC98はV13/64MB/CF-IDE、/sbin空はQEMU上とユーザーが確認した。

WS019/WS025の閉鎖、旧Priority削除、q303停止を維持。新しいQueue・実装・build・実機操作なし。媒体/方式と実行範囲の具体化が次の段階。

## 2026-09-11 計画判断（新規Queueなし）

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active Queueと新しいPriority/Focusはない。

決定者: current user。指示原文:

> 計画を更新します。現在のPriorityリストを削除します。WS025の残件はclearedにして、WS025を閉じます。WS019も閉じます。WS006, WS022, WS002を閉じます。

WS025の確認未実施・実機未確認事項は各Phaseの履歴に保持。既存のBug台帳、WS009/WS014保留、Milestoneの判定は変更しない。

## 最新Queueの履歴

最新: [q303](queue-q303.md) — finished / ws025-p038 uncleared。ユーザー停止。
3ビルドは当時PASS、amd64実行試験は中断、PC/AT未実行。現行修正後の合格ではない。
次のQueueは未承認。

[全Queue索引](queues.md)。旧completedは当時の表記を保持し、現在のclearedへ履歴を改書しない。
