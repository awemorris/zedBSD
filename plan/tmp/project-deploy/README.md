# GitHub Projects deployment — 2026-09-10

Project: [zedBSD — Awesome Plan](https://github.com/users/awemorris/projects/2)
Master: [Issue #1](https://github.com/awemorris/zedBSD/issues/1)

対象は既存361 Issues（Master 1、WS 26、Phase 334）。新規Issue/Draftや
実行Queueは作らない。別リポジトリfuelのProject #1には変更していない。
Project #2をzedBSDへリンクし、初期のprivate公開範囲を保持した。

## 表示

| View | 対象 |
| --- | --- |
| [Milestones / Workstreams](https://github.com/users/awemorris/projects/2/views/2) | 26 WS、Primary Milestoneで分類 |
| [Current Focus](https://github.com/users/awemorris/projects/2/views/3) | fg001〜fg003に対応するWS025と4 Phase |
| [Priority](https://github.com/users/awemorris/projects/2/views/4) | Master指定6 WS、WS Priority昇順。完了分も履歴として保持 |
| [Phases](https://github.com/users/awemorris/projects/2/views/5) | 334 Phase、Parent WSで分類 |
| [Current Queue](https://github.com/users/awemorris/projects/2/views/6) | 空。q303終了・停止後の新規Queueなし |
| [Outlook](https://github.com/users/awemorris/projects/2/views/7) | 空。新しい実行候補Queueは作成しない |

## 意味と移行境界

- Objectives/Milestone Goalsの定義はMaster。Primary MilestoneはMG分類の
  カスタムフィールドであり、GitHub native milestoneへの割当ではない。
- Awesome Plan Statusは既存計画の状態を投影。Issueがopenであることから
  未完了とは推定しない。既定Statusは利用しない。
- Status Noteに限定条件を保持する。Phaseのclearedは当該Phaseの範囲の
  完了記録であり、WS/Milestone全体の達成証拠を自動生成するものではない。
- 意味が一意に正規化できない旧Phaseは状態を未設定にし、本文を参照する。
- ws011-p004はVLANキャンセル・bridge Future移管、ws025-p028は採用取消し。
  canceledは達成証拠から除外する。p038はq303停止後の確認残をunclearedで表示。
- WS025の4 Focus Phaseの直接貢献はtraceability/model.jsonに従う。
  Primaryは親WSの所属を保ち、横断的な貢献をRelatedで表示する。
- native sub-issue関係、自動同期、Awesome Planスキルの正式導入は今回の
  Project表示作成とは別。Project項目とIssue本文は今後併せて更新する。

## 証拠

`project.json` / `state.json`に作成済みIDを保存。
`desired.json`が最終投影値、`items-readback.json`が実取得値。
`verification-errors.json`、`view-counts.json`、`views-readback.json`で照合。
元Issueは`issues.json`、Masterの変更前は`master-before.json`に保持。
デプロイスクリプトは今回のスナップショット用であり、汎用同期として再実行しない。

GitHub REST fieldsはusernameで取得できた。viewsの仕様表ではuser_idだが、
実際にはusernameのパスで作成できた。数値user_idパスは404で変更なし。
作成後はGraphQLで表示・フィルターを取得して確認する。

## 最終検証

361件のIssue所属と全投影フィールドが一致（verification-errors.jsonは空）。
6ビューの実検索件数は26 / 5 / 6 / 334 / 0 / 0。
グループ化・Priority昇順・リポジトリ関連付け・privateを読戻し確認済み。
Masterの変更も読み戻して本文一致を確認した。

状態を推測せず未設定とした旧Phase（条件付き完了・置換済み）: ws003-p017, ws003-p025, ws004-p028, ws004-p029, ws004-p030。
元の記述はStatus NoteとIssue本文に保持。
