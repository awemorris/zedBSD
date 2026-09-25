<!-- awesome-plan project=zedbsd record=ws061p009 -->

# ws061-p009: `cc t.c -o t` を host と同等以上に

Phase ID: `ws061-p009`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q445-i01（uncleared: 2026-09-26 ユーザーの優先の変更で、着手の前に中断）、q447-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー「ccの実行時間もホストと同等以上に速くなることを目標にして、改修を進めてください。」

今: guest 88〜109 ms、host 83〜86 ms（ws061-p008）。

## 受け入れ

`cc t.c -o t`（温まった状態の 5 回）が host（同じ時に 3 回）と同等以上。回帰（boot、make・sh の差分試験、SMP、COW）。

## 進行の記録（q445-i01、2026-09-26）

調べたこと（着手の前）: 1 回あたり guest 99 ms（user 49.5・system 46.5 ms）、host 84 ms（user 44.6・system 40 ms）。compile だけ guest 36・host 30 ms、link だけ guest 62.5・host 50.7 ms。**host の clang は LLVM を静的に link した 1 つの実行 file、guest の clang（145 KB）は `libLLVM.so.23.1`（81 MB）と `libclang-cpp.so.23.1`（79 MB）を動的に読む**（起動のたびに再配置 23 万）。guest の clang を静的に link するのが本筋の候補。

2026-09-26 ユーザーの優先の変更（`syscall`/`sysret` と adaptive spin を先に）で、ここで中断。

## 進行の記録（q447-i01、2026-09-26）

### 調べたこと（QEMU 8 GiB 4 vCPU NVMe、ws061-p010 の後の image）

host の比較の相手は同じ LLVM 23 の clang（`build/llvm-build/bin/clang`、LLVM を静的に link、link は GNU ld）。host の `cc` は gcc（77 ms）、`/usr/bin/clang` は clang 19（124〜133 ms）で、どちらも比較の相手にしない。

| 測定（20 回の平均） | guest | host |
| --- | --- | --- |
| `cc t.c -o t` | 90〜99 ms | 81〜85 ms |
| `cc -c t.c`（compile だけ） | 29.5 ms | 31 ms |
| `cc t.o -o t`（link だけ） | 57〜60 ms | 50 ms |
| `clang --version` | 21 ms | 18.5 ms |
| `ld.lld --version` | 15 ms | 12.5 ms |

compile は host より速く、差は起動（動的 link）にあった。host の `perf kvm --guest` で `cc` のループを測ると、guest の CPU の時間の 42% が `/lib/ld.so` の symbol の探索（`lookup_in_object_version` 18%、`rtld_strcmp` 6.7%、`apply_value`・`relocate_object`・`object_contains` 12%）。原因:

- 名前の hash（SysV と GNU）を探す object ごとに計算し直していた（1 回の探索で 7 object、C++ の名前は 50〜100 文字）。
- 予約の loader の symbol の判定が探索のたびに 17 回の `strcmp`。
- 再配置 1 件ごとに書き込み先の segment を program header の走査で確かめていた（`libclang-cpp` と `libLLVM` の RELATIVE は 23 万件）。

`libLLVM.so` と `libclang-cpp.so` は GNU hash を持ち、symbol の再配置は合わせて約 1.1 万件（`-Bsymbolic-functions`）で、静的な link に替えなくても loader を直せば足りると判断した（静的な link は image を大きくし、LLVM の build をやり直す）。

### 変えたこと

| 内容 | 場所 |
| --- | --- |
| `cc`・`ld` を sh の script から `clang`・`ld.lld` への symbolic link に、`clang++` を複写から link に（`ZEDBSD_PACKAGE_LINKS`）。1 回の compile で sh の起動 1 回（約 1.7 ms）が減る。`files/cc`・`files/ld` を削除 | `userland/packages/lang/clang/Makefile` |
| 探索の名前の 2 つの hash を 1 回の走査で 1 度だけ計算し、探す object の間で使い回す（`struct rtld_symbol_name`、`hash_symbol_name`、`lookup_in_object_hashed`） | `src/rtld/rtld.c` |
| 予約の loader の symbol の判定は、名前が `__` で始まらなければすぐ返す | `src/rtld/rtld.c` |
| 再配置の書き込み先の検査は、直前に当たった書き込み可能な segment を object に覚え、範囲内なら走査を省く（`relocation_target_writable`） | `src/rtld/rtld.c` |
| `spin_trylock` は保持中の lock に交換（`xchg`）を試みず 0 を返す（test-and-test-and-set）。`spin_lock` の回転で保持者の cache line を奪わない | `src/kern/lock.c` |

### 結果（QEMU 8 GiB 4 vCPU NVMe、host は Linux 64 core・同じ clang 23）

| 測定 | 前（p010 の後） | 後 | host（同じ時） |
| --- | --- | --- | --- |
| `cc t.c -o t`（20 回 × 5〜10 組） | 90〜99 ms | **75〜85 ms**（中央 77 ms） | 83〜85 ms |
| `clang --version` | 21 ms | 16〜17 ms | 18.5 ms |
| `ld.lld --version` | 15 ms | 12.5〜13 ms | 12.5 ms |
| configure（`/root`、3 回） | 9.78〜10.19 秒 | **9.10〜9.36 秒** | 10.66〜10.81 秒（clang 23） |
| make（直列、2 回） | 12.6〜12.77 秒 | 12.28〜12.80 秒 | `-j1` 15.2 秒 |

`perf` で ld.so の割合は 42% → 32%（残りは hash の計算そのものと bloom の読み。clang の実行 file は `--hash-style=sysv` で、名前ごとに SysV の hash も要る）。

### 回帰

| 確認 | 結果 |
| --- | --- |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-p009/login.png`） |
| loader（`/bin/dyntest`） | 全項目（STARTUP・RELOC・DLFCN・TLS・PTHREAD-TLS・PLUGIN-OPEN・STDIO-BUFFERING・PLUGIN-TLS）、rc 0。`dlopen("/lib/libc.so")` と `dlsym` |
| C の compile と実行、lldb・ssh・clang++ の起動 | OK |
| make の差分試験 | 91/91（`/root` に置いて。`/tmp` では下の BUG-052 の限界で 72/91） |
| SMP（`smpstress` 6 回）、COW、itimer | 6/6、OK、OK |

`/tmp` の tmpfs は inode の共通の pool（`INODE_COMMON_MAX` 512、devfs と共有）で約 490 file までしか置けず、それを超えると `/dev/null` も開けず sshd が落ちると分かった（BUG-052 に追記、今回の変更と無関係。`3722d91a` からの限界）。

実機は未実施。

## 結果（2026-09-26、cleared）

`cc t.c -o t` は guest 75〜85 ms で host（同じ clang 23）の 83〜85 ms と同等以上。configure も 9.1〜9.4 秒（host 10.7 秒）に縮んだ。clang の静的な link は要らなかった。残る loader の費用（clang の実行 file の SysV hash、同じ symbol の重複の探索）は Future Work の F-017。
