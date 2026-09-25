<!-- awesome-plan project=zedbsd record=ws041 -->

# WS041: 起きた thread を即時に走らせる（interactive の応答）

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-24（q358〜q361）
Primary Milestone: MG006
Related Milestones: なし
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

入力・I/O の完了・timer で起きた thread が、走っている thread の quantum が尽きるのを待たずに走れるようにし、quantum をミリ秒で決める。

## 結果

起床時の preempt と quantum のミリ秒化を実装した。busy loop の横の pipe の往復が 10 ms から 0.13 ms、端末の echo が 4 ms から 0.6 ms、pc98 が 100 ms から 2 ms になった。設計は [design.md](design.md)。

## 制限・移管

測定の道具は `plan/tools/latency/` に移した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws041-p001 | 設計: 起床の preempt と quantum のミリ秒化 | cleared（q358） |
| ws041-p002 | 実装: 起床の preempt と quantum のミリ秒化 | cleared（q359） |
| ws041-p003 | 測定: 起床の遅れ（端末の入力の echo を含む） | cleared（q361） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws041/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
