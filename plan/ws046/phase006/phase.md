<!-- awesome-plan project=zedbsd record=ws046p006 -->

# ws046-p006: kernel の mkdir(2) の `.`・`..`（BUG-032）

Phase ID: `ws046-p006`
Parent: [WS046](../ws.md)
Status: cleared（q402-i01、2026-09-24）
Queue: q402（q402-i01）

## 目的

[BUG-032](../../bugs/BUG-032.md) を直す: mkdir(2) の最後の成分が `.` か `..` のとき、kernel は EINVAL を返すが、POSIX は既にある path に EEXIST を求める。
`mkdir -p ./x` と automake の `$(MKDIR_P) ./$(DEPDIR)` が止まる。

## 設計

`src/kern/syscall.c` の `sys_mutation_common()` の mkdir（mkdirat も同じ道）で、`namei_parent_path_at()` が EINVAL を返し、最後の成分が
`.` か `..` のとき、path 全体を `namei_path_at()` で引き、引ければ EEXIST、引けなければその error（ENOENT など）を返す。
他の syscall（rmdir の `.` は POSIX でも EINVAL、unlink など）は変えない。HAL ではない。

## 受け入れ

- guest で `mkdir -p ./a ./.b x/./y .` が status 0、`mkdir .` と `mkdir ..` が EEXIST（`mkdir: .: File exists`）、`mkdir nothere/.` が ENOENT、`rmdir .` は EINVAL のまま。
- make の差分試験の automake の case が guest で 15/15。kernel の build（warning 0）と boot test。

## 結果（q402-i01、2026-09-24）

`src/kern/syscall.c` の `sys_mutation_common()` に `mkdir_dot_error()` を足した: mkdir（mkdirat も同じ道）で `namei_parent_path_at()` が EINVAL を返し、
最後の成分が `.` か `..` のとき、path 全体が引ければ EEXIST、引けなければその error を返す。他の syscall は変えていない。HAL ではない。

## 検証

| 検証 | 結果 |
| --- | --- |
| guest（amd64、QEMU） | `mkdir -p ./a ./.b x/./y . ..` が status 0。`mkdir .`・`mkdir ..` は `File exists`、`mkdir nothere/.` は `No such file or directory`、`rmdir .` は `Invalid argument` のまま |
| make の差分試験（guest） | automake **15/15**（修正前 14/15）、posix 45/45、gnu 17/31（p003 の範囲）（`evidence/guest-q402.txt`） |
| build | amd64 の guest image（kernel を含む）、warning 0 |
| boot test（amd64） | login prompt（`build/boot-test-amd64-q402/login.png`）。画面の先頭に BUG-031（console の行の混ざり）がまた出た |
| 規約 | 新しい関数に `style-check.py` の違反 0（`sys_mutation_common()` の既存の後始末への goto 2 件は規約 14 で許される形） |
| 実機 | 未実施 |
