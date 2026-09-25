<!-- awesome-plan project=zedbsd record=ws016 -->

# WS016: 実行時の swap 制御

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-08-28（q021）
Primary Milestone: MG004
Related Milestones: なし
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

起動後に swap を追加・削除できるようにする。

## 結果

runtime swap manager、`/dev/system` の UAPI、`/sbin/swapon`・`/sbin/swapoff` を実装し、QEMU で受け入れた。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws016-p001 | runtime swap manager | cleared（q021） |
| ws016-p002 | `/dev/system` runtime-swap UAPI | cleared（q021） |
| ws016-p003 | `/sbin/swapon` and `/sbin/swapoff` | cleared（q021） |
| ws016-p004 | runtime-swap QEMU acceptance | cleared（q021） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws016/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
