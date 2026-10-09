<!-- awesome-plan project=zedbsd record=ws196 -->

# WS196: useradd・usermod・userdel（root の端末の利用者の管理）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Related Milestones: —
Objectives: O1
Parent: [Master](../master.md)
Queue: 未割当
Target: ベータ3 以降（2026-10-09 Q1 の提案、時期はユーザーの判断で確定）
Resume point: p001 の E1〜E6（ユーザーの判断、[phase.md](phase001/phase.md) §4）。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「account-admin コマンドは、useradd/mod/delと同等のものでしょうか？useradd/usermod/userdelの方がユーザは使いやすいと思うのですが」「useradd/usermod/userdelは別途、実装が必要な認識ですが」

## 目標

- root が端末で使う useradd・usermod・userdel を base に足す。POSIX には利用者の管理の utility は無い（passwd も POSIX に無い）ので、Linux の shadow-utils の option の慣習に合わせる（範囲は p001 で決める）。
- /etc/passwd・/etc/shadow・/etc/group の編集は account-admin の edit.c と共有する（WS195 で account-admin が Keiland へ移るので、共有の置き場も p001 で決める）。
- account-admin の安全の規則（root・system の account の保護、最後の管理者）との関係を決める。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 範囲（option）と共有の code の置き場の設計 | planning（2026-10-09 夜 P1: 調べと設計の案。E1〜E6 のユーザーの判断と design-reviewer 待ち。p002〜p005 の案は phase.md §5） | WS195 p001 |
