<!-- awesome-plan project=zedbsd record=ws002 -->

# WS002: システムサービス

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-11（p021 をユーザー判断で cleared、WS を閉鎖）
Primary Milestone: MG005
Related Milestones: MG002
Objectives: O1, O2, O3
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

init とシステムサービス（service 管理、syslogd・logger、getty・login、cron・at、ntpdate、networkd・net）を実装し、最小のシステムとして QEMU で受け入れる。

## 結果

init・service・ログ・login session・cron・networkd を実装し、統合 QEMU 受け入れを通した。USB 起動の halt 時の失敗（BUG-010）は QEMU で再現・修正（p023）、退役時の heap 破損は p024 で修正した。

## 制限・移管

p021（login 不在時の crash loop）はユーザー判断で cleared。当時の原因は未証明で、再発条件は BUG-012 に残す。`/bin/sh` の POSIX 互換性は WS042 が引き継いだ。実機の EHCI/UHCI での BUG-010 の追加確認は残る。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws002-p011 | service foundation and administrative layout | cleared |
| ws002-p012 | native init and service lifecycle | cleared |
| ws002-p013 | logger and zedBSD syslogd | cleared |
| ws002-p014 | getty, login sessions, and respawn | cleared |
| ws002-p015 | initial networkd and net | cleared |
| ws002-p016 | cron, crontab, at, and batch | cleared |
| ws002-p017 | optional boot-time ntpdate | cleared |
| ws002-p018 | POSIX.1-2024 `/bin/sh` | cleared（最小の sh。互換性の残りは WS042 が担う） |
| ws002-p019 | integrated QEMU acceptance and repair | cleared |
| ws002-p020 | synchronous net service and networkd command orchestration | cleared |
| ws002-p021 | missing-login session teardown and crash-loop robustness | cleared |
| ws002-p022 | intermittent console-login progress | cleared |
| ws002-p023 | USB boot halt failure | cleared（q135） |
| ws002-p024 | retirement heap integrity | cleared（q142） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws002/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
