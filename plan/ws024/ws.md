<!-- awesome-plan project=zedbsd record=ws024 -->

# WS024: 64-bit UFS の実装の一本化

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-07（q102）
Primary Milestone: MG004
Related Milestones: なし
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

UFS の実装を 64-bit の 1 つにまとめ、formatter と image の利用者をそれへ移す。

## 結果

統一した UFS の形式と移行の契約（[format-contract.md](format-contract.md)）、単一の 64-bit UFS driver、formatter と image の利用者の移行、旧経路の撤去を行い、U01〜U24 の受け入れを通した。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws024-p001 | unified UFS format and migration contract | cleared（q100） |
| ws024-p002 | single 64-bit UFS driver | cleared（q101） |
| ws024-p003 | UFS formatters and image consumers | cleared（q101） |
| ws024-p004 | UFS acceptance and retired-path removal | cleared（q102） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws024/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
