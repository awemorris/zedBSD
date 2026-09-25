<!-- awesome-plan project=zedbsd record=ws035p033 -->

# ws035-p033: 設計 — kernelからlibcを切り離す

Phase ID: `ws035-p033`
Parent: [WS035](../ws.md)
Status: cleared（q317-i02、2026-09-23。設計 [kcrt-design.md](../kcrt-design.md)、レビュー [review.md](review.md)）
Phase disposition: normal
Queue: q317（q317-i02）
実行: `driver-designer`（Fable 5.1、High）→ 敵対的レビュー `design-reviewer`（Fable 5.1、High）

## 目的

kernel（vmunix）がlibcのsourceをlinkし、libcの公開ヘッダを読んでいる状態を根本からやめるための設計を作る。
実装はp034（kcrtとheap）、p035（include整理）で行う。**この Phase ではソースを変更しない。**

## 前提となる事実（ws035-p001と2026-09-23の調査）

- vmunixは `libc/libc.mk` の `ZEDBSD_LIBC_SOURCES`（heap、string、stdio、locale、regex、ndbm、realpath等）をlinkしている
  （`platform/amd64/vmunix.mk` の `AMD64_KERNEL_LIBC_OBJS`）。kernelのcompileは `-isystem $(sysroot)/usr/include` でlibcの公開ヘッダを読む。
- kernelが実際に使うlibcの関数は24個: heap allocator 9（`heap_allocator_*`、`heap_active_set`）、`memchr`・`memcmp`・`memcpy`・
  `memmove`・`memset`・`memset_explicit`、`snprintf`、`strcat`・`strchr`・`strcmp`・`strcpy`・`strlen`・`strncmp`・`strncpy`・
  `strnlen`・`strrchr`。
- kernelが読むlibcヘッダ（i915-amd64）: `string.h` 166、`errno.h` 179、`locale.h` 178、`sys/ioctl.h` 61、`sys/stat.h` 47、
  `time.h` 85、`stdio.h` 4 など。i915のnative Vulkan実行器はVulkanのヘッダも読む。監査は `plan/ws035/phase001/include-audit/`。
- kernelのallocator APIは `include/kern/kmem.h`（`kern_malloc`・`kern_calloc`・`kern_free`・`kern_memory_get_stats`）で、
  `src/kern/entry.c` が `libc/heap.c` の `heap_allocator_*` の上に実装している。

## ユーザーの決定（2026-09-23）

- kernel用Cランタイム（kcrt）のヘッダは `include/kern/kcrt.h`、実装は `src/kern/kcrt.c`。標準C APIの代替を `kern_*()` で作る。
- compilerが暗黙に生成する呼出し（clangは構造体のcopy等を `__builtin_memcpy` 等として扱い、`memcpy`・`memset`・`memmove`・
  `memcmp` の呼出しを出すことがある）は、libcを外したときのlinkエラーから見つけ、その名前の定義を `kcrt.c` に足す。
- kcrtはHALのCランタイム（`hal_memcpy` 等）を呼ばない。すべて `kcrt.c` に自前で実装する。HALのCランタイムはいつでも
  kernelから呼べるが本来は初期化の段階で使うもので、初期化後はkcrtを使う。この使い分けは `kcrt.h` のコメントに書く（`hal.h` は変えない）。
- `libc/heap.c` を複製して `src/kern/heap.c` を作り、以後は別々に保守する。関数名等はkernel用に整理してよい。
- HALは `#include` のパス変更だけ承認済み。HALの宣言・実装・責務は変えない。
- amd64以外のbuildは壊れてもよい。

## 設計に含めること

1. `kcrt.h` のAPI一覧（名前、引数、意味、標準関数との違い）と、既存の呼出しからの置換規則（機械的に置換できる範囲）。
2. `kern_snprintf` の書式の範囲（kernelが実際に使う書式を調べて決める）。既存のkernelのprintf系（`kern_printf` 等があれば）との関係。
3. compilerが暗黙に出す呼出しの扱い（`-ffreestanding`・`-fno-builtin` の有無、定義を置く名前、確かめ方）。
4. `src/kern/heap.c` の構成（複製元との対応、kernel用の名前、`kern_malloc` 側の変更点）。
5. 型と定数の出どころ: errno・ioctl・stat・time・fcntl・termios・socket等は `include/uapi/`、整数型等はcompilerのfreestanding
   ヘッダ（`stdint.h`・`stddef.h`・`stdbool.h`・`stdarg.h`・`limits.h` 等）か `include/kern/` の型。kernelのcompileから
   `-isystem $(sysroot)/usr/include` を外し、freestandingヘッダだけを読める設定にする方法。
6. Vulkanのヘッダ（i915のnative Vulkan実行器）の扱いの案（uapiに置くか、kernel専用の置き場か、例外とするか）。
7. `locale.h` 等、どのkernelファイルがなぜlibcヘッダを読んでいるかの分類（直接のinclude、`string.h` 等からの連鎖）と、
   ファイルごとの置換先。
8. 検査: kernel・HALが読むlibcヘッダが0件であることをbuildで保証する方法（`kernel-include-audit.py` の拡張等）。
9. p034とp035への分け方、各Phaseの受け入れ条件と確認手順（amd64・i915-amd64のbuild、host試験、QEMU起動）。

## 成果物

`plan/ws035/kcrt-design.md`（設計）と、`plan/ws035/phase033/review.md`（敵対的レビューの結果と、それへの対応）。

## 受け入れ

- 設計が上の9項目を含み、既存コードの実際のファイル・行を根拠にしている。
- 敵対的レビューを行い、指摘ごとに「設計を直した／直さない理由」が記録されている。
- ソースに差分が無い。
