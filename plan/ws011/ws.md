<!-- awesome-plan project=zedbsd record=ws011 -->

# WS011: ネットワーク設定 console

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-09（ユーザー確認）
Primary Milestone: MG005
Related Milestones: MG003
Objectives: O1, O2, O3, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

`net.conf` の形式と `net` の対話 console を作り、設定の永続化と、失敗しても元に戻る confirmed commit を提供する。

## 結果

`net.conf` v1 の parser、対話 console、永続化と起動時の移行、confirmed commit の設計・実装・自動と実機の受け入れ、overlay への公開の修正を行った。

## 制限・移管

VLAN は取り消し、bridge は Future Work F-001 へ移した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws011-p001 | `net.conf` v1 format and parser | cleared |
| ws011-p002 | interactive `net` console | cleared |
| ws011-p003 | persistence and boot migration | cleared |
| ws011-p004 | VLAN and bridge interfaces | canceled（VLAN は取消し、bridge は Future Work F-001 へ） |
| ws011-p005 | confirmed-commit design | cleared |
| ws011-p006 | confirmed-commit implementation | cleared（q073） |
| ws011-p007 | confirmed-commit automatic acceptance | cleared（q075） |
| ws011-p008 | confirmed-commit physical acceptance | cleared |
| ws011-p009 | confirmed-commit overlay publication correction | cleared（q075） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws011/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
