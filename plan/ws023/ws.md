<!-- awesome-plan project=zedbsd record=ws023 -->

# WS023: i386/amd64 HAL のコーディング規約準拠

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-03（q067）
Primary Milestone: MG001
Related Milestones: MG008
Objectives: O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

x86 の HAL の source を plan/coding-style.md に合わせる。

## 結果

i386 の core・割り込み・VM・task、PC/AT と PC-98 の BSP、amd64 の各 module・割り込み・page・SMP・task・boot・APIC・time・address space・console・input を規約に合わせ、回帰を監査した。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws023-p001 | Restore compressed i386 HAL core source | cleared（q067） |
| ws023-p002 | Conform the i386 core and interrupt sources | cleared（q067） |
| ws023-p003 | Conform the i386 VM and task sources | cleared（q067） |
| ws023-p004 | Conform the i386 PC/AT BSP | cleared（q067） |
| ws023-p005 | Conform the i386 PC-98 BSP | cleared（q067） |
| ws023-p006 | Conform amd64 leaf and small core modules | cleared（q067） |
| ws023-p007 | Conform amd64 interrupt, page, SMP, and task code | cleared（q067） |
| ws023-p008 | Conform amd64 boot, APIC, and time sources | cleared（q067） |
| ws023-p009 | Conform the amd64 address-space implementation | cleared（q067） |
| ws023-p010 | Conform the amd64 console and input implementation | cleared（q067） |
| ws023-p011 | Complete the x86 HAL style and regression audit | cleared（q067） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws023/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
