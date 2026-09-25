<!-- awesome-plan project=zedbsd record=ws008 -->

# WS008: Noct と BeUI

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-02（q063）
Primary Milestone: MG006
Related Milestones: MG001, MG007
Objectives: O1, O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

Noct（script 言語）と BeUI（GUI toolkit）を zedBSD の標準の構成で build し、graphics・evdev backend と JIT を動かす。

## 結果

CMake preset、BeUI の graphics・evdev backend、amd64 の mmap/mprotect による JIT、host toolchain の版固定、target Noct の `userland/base` への移動と host script の CLI 契約を整えた。

## 制限・移管

Remacs と i386/PC-98 の target Noct は対象外とした。p006（maintainer review）は差し戻され、後続の Phase で置き換えた。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws008-p001 | canonical Noct zedBSD CMake preset | cleared（q019） |
| ws008-p002 | canonical BeUI zedBSD graphics and evdev backend | cleared（q020） |
| ws008-p003 | amd64 Noct mmap/mprotect JIT acceptance | cleared（q020） |
| ws008-p004 | upstream review corrections and source delivery | cleared（q022） |
| ws008-p005 | independent BeUI platform implementations | cleared（q023） |
| ws008-p006 | maintainer API and source-layout review | uncleared（q024。review で差し戻し、後続 Phase で置換） |
| ws008-p007 | disable target Noct package options | cleared（q025） |
| ws008-p008 | pin the latest Noct host toolchain | cleared（q041） |
| ws008-p009 | relocate target Noct under base and resume it | cleared（q063） |
| ws008-p010 | restore the Noct host-script CLI contract | cleared（q063） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws008/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
