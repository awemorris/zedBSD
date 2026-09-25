<!-- awesome-plan project=zedbsd record=ws018 -->

# WS018: kernel の source 所有と interface の統合

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-07（q087）
Primary Milestone: MG008
Related Milestones: MG001, MG004
Objectives: O1, O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

driver・filesystem・boot・input・graphics の source の所有と公開 API を整理し、HAL 移植契約の土台を作る。

## 結果

driver の source tree 移動、disk label と platform の所有、UFS1/UFS2 の独立実装、filesystem の identity probe、core と boot の統合、Xzed の evdev 専用化、graphics frontend の独立、FAT の VFS 移行、旧 bootfs と /diskN の撤去、mount 名前空間の保護、filesystem の I/O 経路と 50 の受け入れ story を完了した。

## 制限・移管

I/O・cache の後続は WS025 が担った。移植契約全体の完成（MG008）はこの WS だけでは証明しない。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws018-p001 | driver source-tree relocation | cleared（q025） |
| ws018-p002 | disk-label and platform ownership | cleared（q026） |
| ws018-p003 | independent UFS1 and UFS2 implementations | cleared（q025） |
| ws018-p004 | filesystem-owned identity probes | cleared（q025） |
| ws018-p005 | core source consolidation | cleared（q025） |
| ws018-p006 | boot implementation and public API consolidation | cleared（q025） |
| ws018-p007 | Xzed evdev-only consumer | cleared（q026） |
| ws018-p008 | independent input/HID driver ownership | cleared（q026） |
| ws018-p009 | independent graphics frontends | cleared（q035） |
| ws018-p010 | FAT source consolidation | cleared（q035） |
| ws018-p011 | FAT native VFS migration | cleared（q035） |
| ws018-p012 | legacy bootfs and startup residue removal | cleared（q035） |
| ws018-p013 | remove legacy /diskN mount scaffolding | cleared（q077） |
| ws018-p014 | mounted namespace mutation protection | cleared（q077） |
| ws018-p015 | filesystem-wide risk audit | cleared（q077） |
| ws018-p016 | Integrate the refactored kernel with current fixes | cleared（q084） |
| ws018-p017 | Storage baseline and image audit | cleared |
| ws018-p018 | Coherent filesystem I/O path | cleared |
| ws018-p019 | Fifty filesystem acceptance stories | cleared |
| ws018-p020 | Request-sized syscall I/O | cleared |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws018/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
