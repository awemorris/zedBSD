# zedBSD Projects展開

状態: 完了（2026-09-10）。[Project #2](https://github.com/users/awemorris/projects/2)へ361件を登録し、全投影フィールドと6ビューを読戻し照合済み。詳細は[展開記録](README.md)。
ユーザー承認範囲: 既存IssuesとトレーサビリティのProjects展開。

## 対象

- owner: awemorris / repository: awemorris/zedBSD
- 既存Projectを調べ、対応するものがなければ `zedBSD — Awesome Plan` を作成。
- [items.json](items.json)の既存Issue 361件を登録。Draft Issueの複製は作らない。
- Issue本文が目的・状態・根拠の正本。Projectは一覧・絞込みのための表示。
- Projectの公開範囲は作成前に確認する。リポジトリが公開でも勝手にProjectを公開へ変更しない。

## フィールド

Logical ID、Record Kind、Primary Milestone、Related Milestones、Objectives、
Awesome Plan Status、Phase Disposition、WS Priority、Focused Goal IDs、
Queue ID、Queue Item Status、Planning Categoryを、既存の互換フィールドを再利用して設定。
MilestoneはまずMG IDを保持するフィールドで表現し、native milestoneとは区別する。
既存Projectへnative milestoneが必要なら別途実際のIssue割当との整合を取る。

状態は現行Issue本文とローカル記録を照合して設定する。openを未完了、closedを
完了と機械変換しない。不明な状態は未設定として対応表に残し、架空のDoneにしない。
p028はcanceled。q303はfinishedであり、Current Queueのactive項目を捏造しない。
Priorityの既存6項目の順序と、完了したWSが含まれる事実は区別して示す。

## 表示

1. Milestones / Workstreams: WSをPrimary Milestoneで分類。
2. Current Focus: Focused Goalsに対応するWS/Phase。
3. Priority: WSを既存Priority順に表示。
4. Phases: WS/状態/取消しで絞り込める一覧。
5. Current Queue / Outlook: 実在する計画のみ。新規Queueは作らない。

利用できるCLI/APIを確認し、viewの作成・filter/group/sortの設定と読戻しまで行う。
利用APIで設定不能な項目は未設定として報告し、Project作成だけで全面完了としない。
Bug/Queue historyのIssue化は今回の361件登録に含まれていないため架空の項目を作らない。

## 再開条件と検証

`gh auth refresh -h github.com -s project` をユーザー側で承認後に再開。
既存Projectの全件確認 → field準備 → items追加 → fields設定 → views設定 → 読戻し。
途中結果のIDを保存して重複作成を避ける。Issue本文や既存ユーザー設定を上書きしない。
