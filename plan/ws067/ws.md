<!-- awesome-plan project=zedbsd record=ws067 -->

# WS067: `/dev/fd` を呼んだ process の descriptor に合わせる（BUG-054）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG004
Objectives: O1
Parent: [Master](../master.md)
Queue: q455（ws067-p002）
Resume point: p001 cleared（BUG-054 resolved）。p002（規約）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー「BUG-054の修正を最優先」。[BUG-054](../bugs/BUG-054.md): `/dev/fd/N` が開いた file でなく文字 device に見え、`diff <(..) <(..)` が「device identity」で違うと言い、`ls /dev/fd` が 1024 の node を作って devfs の inode の pool を尽くし sshd が落ちる。

受け入れ: `/dev/fd` の一覧と lookup は呼んだ process が開いている descriptor だけ。`stat`・`lstat` の `/dev/fd/N`（と `/dev/stdin`・`stdout`・`stderr`）は `fstat(N)` と同じ。`ls /dev/fd`・`ls -l /dev/fd` で inode が尽きず sshd が生きている。`diff <(echo 1) <(echo 1)` は status 0、`diff <(echo 1) <(echo 2)` は差を出して status 1。`/dev/fd/N` の open（dup と同じ）は今のまま。規約。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws067-p001](phase001/phase.md) | devfs の `/dev/fd` の一覧と lookup、stat の descriptor への置き換え、`diff` の FIFO・文字 device の中身の比較 | cleared（q454-i01。guest の再現手順が全て期待どおり、sh の差分 1412/1458 で変わらず、boot PASS） | — |
| [ws067-p002](phase002/phase.md) | 規約の適合（WS067 で変えた source） | planned | p001 |
