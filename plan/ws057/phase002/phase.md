<!-- awesome-plan project=zedbsd record=ws057p002 -->

# ws057-p002: 長さの 32 bit 切り捨て（BUG-048）の修正と `MAP_NORESERVE` の受け付け

Phase ID: `ws057-p002`
Parent: [WS057](../ws.md)
Status: cleared
Queue: q426-i01

## 目的

[p001](../phase001/phase.md) の設計 1・2: `src/kern/syscall.c` の `SYSCALL_PAGE_MASK` と `src/kern/vmspace.c` の 5 箇所の丸めを 64 bit にし、`MAP_NORESERVE` を `<uapi/mman.h>` に定義して mmap が受け付けて無視するようにする。HAL は変えない。

## 受け入れ

- probe: 64 GiB の `PROT_NONE` の reserve が成功して commit は増えない。5 GiB の RW の mapping の offset 4 GiB + 4 KiB を触れる。8 GiB の RW（上限 8237 MiB 超）は **ENOMEM**。`mprotect(len = 4 GiB + 4 KiB)` は 2 GiB の region で失敗する。`MAP_NORESERVE` 付きの mmap が成功し commit は通常どおり課金される。
- 規約。回帰: amd64 の kernel の build と boot、guest の sh・make の差分試験、`SMP-STRESS.ELF`。

## 実装（q426-i01、2026-09-25）

- `src/kern/syscall.c`: `SYSCALL_PAGE_MASK` を `((uintptr_t)KERN_PAGE_SIZE - 1U)` に（`~` が 64 bit になり、mmap・munmap・mprotect など 9 箇所の丸めが直る）。mmap の flag の検査に `MAP_NORESERVE` を足して受け付け、動作は変えない（commit は通常どおり課金）。
- `include/uapi/mman.h`: `MAP_NORESERVE 0x4000`（Linux と同じ値）。
- `src/kern/vmspace.c`: `& ~(PAGE_SIZE - 1U)` の 5 箇所（2554・5132・5220・5254・5294 行）を `~(uintptr_t)(PAGE_SIZE - 1U)` に。
- HAL は不変。規約: 変えた行は `style-check.py` の報告 0。amd64 の kernel の build は warning 0。

## 結果（q426-i01、2026-09-25。QEMU amd64 8 GiB、swap 64 MiB、作り直した image）

| 検証 | 結果 |
| --- | --- |
| build（disk image、full LTO。uapi の変更で guest の LLVM も再 build） | status 0、我々の code の warning 0 |
| boot test | PASS、`build/boot-test-amd64-q426/login.png` |
| `PROT_NONE` の reserve（`commitprobe2`） | 1〜**128 GiB** まで成功、commit は増えない（前は 4 GiB 以上が EINVAL） |
| 64 GiB の reserve の 2 GiB を `mprotect` で RW | 成功、used +2048 MiB、offset 2 GiB − 4 KiB を触れる |
| 5 GiB の RW の mapping | 成功、used +5120 MiB、**offset 4 GiB + 4 KiB を触れる**（前は SIGSEGV） |
| 上限（8237 MiB）超の commit | 8 GiB（8192 MiB）は上限内で成功（probe の期待が誤り）、**9 GiB は ENOMEM**（前は 1 GiB として「成功」） |
| `mprotect(len = 4 GiB + 4 KiB)`（2 GiB の region） | **失敗**（EINVAL。前は 4 KiB として成功） |
| `MAP_NORESERVE` 付きの 1 GiB RW | 成功（前は EOPNOTSUPP）、used +1024 MiB（commit は通常どおり） |
| `munmap` の後 | fork した子（touch の試験）の分が非同期に返るまで残り、最後は delta 0 |
| `SMP-STRESS.ELF`（起動 5 秒後）・make の差分試験 | status 0・91/91 |
| sh の差分試験 | 1388/1425、落ちる 37 件は q422 と同じ集合 |
| 実機 | 未実施 |

### 受け入れの判定

受け入れの probe は全て満たした（8 GiB は上限内なので成功が正しく、9 GiB で ENOMEM）。規約 0、kernel の build と boot、sh・make の差分試験、SMP stress を通した。q426-i01 は **cleared**。BUG-048 は resolved。
