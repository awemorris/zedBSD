<!-- awesome-plan project=zedbsd record=ws020 -->

# WS020: Intel Mac の UEFI 起動と Variant

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-05（ユーザーの実機確認）
Primary Milestone: MG003
Related Milestones: MG008
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

Intel Mac の UEFI から起動できる image と、target Variant の仕組みを作る。

## 結果

汎用の target Variant、amd64 の BIOS・複合・Apple UEFI の image layout、QEMU の Variant 行列、大きな媒体の preflight、GPT と protective MBR の優先を実装し、ユーザーが Intel Mac の実機で起動を確認した。

## 制限・移管

なし。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws020-p001 | generic target Variant | cleared |
| ws020-p002 | amd64 BIOS, combined, and Apple UEFI image layouts | cleared |
| ws020-p003 | complete QEMU Variant matrix | cleared（q047） |
| ws020-p004 | Intel Mac physical UEFI bring-up | cleared |
| ws020-p005 | production UEFI-only larger-media preflight | cleared（q038） |
| ws020-p006 | GPT precedence over Protective-MBR extent | cleared |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws020/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
