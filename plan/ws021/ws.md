<!-- awesome-plan project=zedbsd record=ws021 -->

# WS021: 再現可能な x86 LLVM toolchain と sysroot

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-02（q064）
Primary Milestone: MG001
Related Milestones: MG002
Objectives: O1, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

LLVM を zedBSD target 付きで build し、amd64・i386 の sysroot とともに kernel・userland・loader の build をそれへ移す。

## 結果

LLVM の取得と zedBSD target の patch、host での build と install、x86 の sysroot、kernel・userland・target Noct・BIOS/UEFI loader の移行、x86 の一括 QEMU 受け入れ、Linux host 用 release を完了した。

## 制限・移管

AArch64 の zedbsd target と sysroot は WS036 p026 が担う。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws021-p001 | LLVM source acquisition and zedBSD target patch | cleared（q064） |
| ws021-p002 | host LLVM build and installation | cleared（q064） |
| ws021-p003 | amd64 and i386 sysroots | cleared（q064） |
| ws021-p004 | x86 kernel, userland and target Noct migration | cleared（q064） |
| ws021-p005 | BIOS/UEFI loader and disk-image toolchain closure | cleared（q064） |
| ws021-p006 | consolidated x86 big-bang QEMU acceptance | cleared（q064） |
| ws021-p007 | permanent x86_64 Linux host LLVM release | cleared（q064） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws021/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
