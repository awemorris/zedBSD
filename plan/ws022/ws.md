<!-- awesome-plan project=zedbsd record=ws022 -->

# WS022: ELF の PT_TLS と static TLS

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-11（q128、ユーザー指示で閉鎖）
Primary Milestone: MG002
Related Milestones: なし
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

ELF の thread-local storage（PT_TLS）を loader と pthread で扱えるようにする。

## 結果

TLS の ABI 契約と ELF fixture、実行ファイルの TLS の読み込みと初期 thread、pthread の TLS の寿命を実装し、amd64・i386 の static TLS と dynamic の回帰、PC-98 で確認した。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws022-p001 | TLS ABI contract and ELF fixtures | cleared（q127） |
| ws022-p002 | executable TLS loading and initial thread | cleared（q128） |
| ws022-p003 | pthread TLS lifetime and runtime acceptance | cleared（q128） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws022/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
