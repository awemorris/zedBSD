<!-- awesome-plan project=zedbsd record=ws067 -->

# WS067: `/dev/fd` を呼んだ process の descriptor に合わせる（BUG-054）

<!-- awesome-plan-current:start -->
Status: completed（2026-09-26）
Primary Milestone: MG002
Related Milestones: MG004
Objectives: O1
Parent: [Master](../master.md)
Queue: q455（finished）
Resume point: —（完了）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー「BUG-054の修正を最優先」。[BUG-054](../bugs/BUG-054.md): `/dev/fd/N` が開いた file でなく文字 device に見え、`diff <(..) <(..)` が「device identity」で違うと言い、`ls /dev/fd` が 1024 の node を作って devfs の inode の pool を尽くし sshd が落ちる。

## 結果

- `src/kern/devfs.c`: `/dev/fd` の一覧と、`/dev/fd/N`・`/dev/stdin`・`stdout`・`stderr` の lookup は、呼んだ process が持つ descriptor だけ（持たない N は ENOENT で node を作らない。一覧の型は descriptor の file の型）。4.4BSD の fdescfs・Linux と同じ見え方。
- `src/kern/syscall.c`: `stat`・`lstat`・`fstatat` の `/dev/fd` の node は、その descriptor の `fstat` と同じ（`descriptor_getattr()` を `fstat` と共有）。`/dev/fd/N` の open（dup と同じ）は変えていない。
- `userland/base/diff/`: operand の一方でも FIFO か文字 device なら、両方を一時 file に写して比べ、出力は operand の名前で出す。
- 検証（QEMU 8 GiB 4 vCPU NVMe）: BUG-054 の再現手順が全て期待どおり（`ls /dev/fd` は開いた descriptor だけ、`ls -lL` は regular・pipe を正しく、`diff <(echo 1) <(echo 1)` は 0・`<(echo 2)` は 1、`ls -l /dev/fd` の 50 回の繰り返しの後も SSH が生きる、一時 file は残らない）。sh の差分試験 guest 1412/1458（変わらず、`diff <(..) <(..)` の case を戻した）。build warning 0、boot test PASS（p001・p002）。規約: style-check の指摘は変えた行で 0、全文の規約で見直した。
- 実機は未実施。

## 制限・移管

- `ls /dev/fd` は ls が開いた directory の descriptor を出さない（一覧は directory の open の時に作る。Linux は出す）。
- base の `diff` は行ごとの比較で、最長共通部分列の diff ではない（出力は POSIX の形でない）。WS067 の範囲外で、既存の制限。
- devfs の inode の共通の pool の大きさは BUG-052 のまま。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws067-p001 | devfs の `/dev/fd` の一覧と lookup、stat の descriptor への置き換え、`diff` の FIFO・文字 device | cleared（q454-i01） |
| ws067-p002 | 規約の適合 | cleared（q455-i01。変えた行の style-check 0、全文で見直し、guest の再確認、boot PASS） |
