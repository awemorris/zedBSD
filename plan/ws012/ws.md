<!-- awesome-plan project=zedbsd record=ws012 -->

# WS012: サービス管理 console

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-08-28（q018）
Primary Milestone: MG005
Related Milestones: MG003
Objectives: O1, O2, O3, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

サービスの設定・起動・停止を、`rc.conf` と init の制御 protocol を通して一貫して行えるようにする。

## 結果

YAML の rc.conf と永続化、init の ZSV1 service 制御 protocol、非対話の `service` CLI と永続の方針、対話の console を実装し、統合受け入れを通した。

## 制限・移管

container を使うサービスは WS013（Future Work F-002）の範囲。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws012-p001 | service-console design discussion | cleared |
| ws012-p002 | YAML rc.conf model and persistence foundation | cleared（q017） |
| ws012-p003 | ZSV1 init service-control protocol | cleared（q018） |
| ws012-p004 | non-interactive service CLI and persistent policy | cleared（q018） |
| ws012-p005 | interactive service console | cleared（q018） |
| ws012-p006 | service-console integration acceptance | cleared（q018） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws012/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
