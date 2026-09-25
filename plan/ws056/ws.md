<!-- awesome-plan project=zedbsd record=ws056 -->

# WS056: POSIX の試験と utility の小さな不具合を直す（BUG-034・035・037、実行中に見つけた BUG-042・043）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q423
Resume point: p001 uncleared（残りは BUG-046 だけ）。ユーザーの判断で p001 の clear（BUG-046 を non-blocking）か p002
<!-- awesome-plan-current:end -->

## 目標

- [BUG-034](../bugs/BUG-034.md): `RTSIG_MAX`（34）が `SIGRTMAX - SIGRTMIN + 1`（33）と合わない。`<uapi/limits.h>` を 33 にする（kernel は他で使っていない）。`POSIX-R2.ELF` が status 0。
- [BUG-035](../bugs/BUG-035.md): `posix-r2-remaining.c` の `struct atomic_record` が 12 byte で `_Atomic` が lock-free でない。`left`・`right` を `uint16_t` にして 8 byte に。compile でき guest で status 0。
- 実行中に見つけて同じ Phase で直す: [BUG-042](../bugs/BUG-042.md)（libc の SIGEV_THREAD の worker の signal mask。`POSIX-R2.ELF` の status 0 に要る）、[BUG-043](../bugs/BUG-043.md)（scm-emfile の試験の前提。`POSIX-R2-REMAINING.ELF` の status 0 に要る）。
- [BUG-037](../bugs/BUG-037.md): pax の読み手に typeflag `x`・`g`（pax の拡張 header: path・linkpath・size・mtime）と `L`・`K`（GNU の長い名前）を足す。GNU tar の `--format=pax` と `--format=gnu` の archive を展開できる。

きっかけ: 2026-09-25 ユーザー「報告してくれた未修正の問題と、新しいバグについて、解決に取り組んでください。」

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws056-p001](phase001/phase.md) | 3 件の修正と試験、規約の確認 | uncleared（q423-i01。BUG-034・035・037・043・044 を直し、BUG-042 の真因（thread の mask の継承）を kernel と libc で直した。pax は pax・gnu の archive を guest で展開して host と一致、`POSIX-R2-REMAINING.ELF` は 01-12 PASS。残りは `POSIX-R2.ELF` が console で timer の試験の EINTR（BUG-046）で status 0 にならないこと） | — |
| ws056-p002（案） | console での `POSIX-R2.ELF` の EINTR の経路の特定（BUG-046） | planning（ユーザーの判断待ち: BUG-046 を non-blocking として p001 を clear するか、p002 を立てるか） | p001 |
