<!-- awesome-plan project=zedbsd record=ws010 -->

# WS010: Noct の script と build tool

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-02（q063）
Primary Milestone: MG001
Related Milestones: MG007
Objectives: O1, O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

build・image 作成の tool を host の Noct で書き、x86 の build をそれへ移す。

## 結果

host Noct の toolchain、x86 の build tool の移行、依存の閉包の監査、3 platform の起動受け入れ、userland の source 配布の lifecycle を整えた。WS021 がこの bootstrap を使う。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws010-p001 | Noct host toolchain | cleared |
| ws010-p002 | x86 build-tool migration | cleared |
| ws010-p003 | x86 dependency-closure audit | cleared |
| ws010-p004 | three-platform boot acceptance | cleared |
| ws010-p005 | userland source-distribution lifecycle | cleared（q063） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws010/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
