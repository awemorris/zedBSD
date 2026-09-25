<!-- awesome-plan project=zedbsd record=ws006 -->

# WS006: 入力と evdev

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-11（q147、ユーザー指示で閉鎖）
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

入力 device を evdev 互換の event 体系にまとめ、USB HID・PS/2 の producer と X server・console などの consumer をつなぐ。

## 結果

input core と event device、capability と状態の query、USB HID の descriptor・report・hotplug、物理 key の broker、旧 console 入力の撤去、graphical PTY の terminal identity を実装した。xHCI と EHCI/UHCI の両構成で USB root・HID・hotplug・Xzed の入力を受け入れた。

## 制限・移管

port ごとの物理 key の対応は機種の作業に残した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws006-p001 | evdev compatibility profile | cleared |
| ws006-p002 | input core and event device | cleared |
| ws006-p003 | existing input producer bridge | cleared |
| ws006-p004 | physical-key input broker and guest evidence | cleared |
| ws006-p005 | evdev capability and current-state queries | cleared（q020） |
| ws006-p006 | input truthfulness and multi-source ownership | cleared（q044） |
| ws006-p007 | USB HID descriptor and report core | cleared（q044） |
| ws006-p008 | USB HID evdev producers and hotplug | cleared（q048） |
| ws006-p009 | consumer closure and legacy console-input removal | cleared（q147） |
| ws006-p010 | EHCI/UHCI USB-root recovery | cleared（q147） |
| ws006-p011 | terminal identity for graphical PTYs | cleared（q147） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws006/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
