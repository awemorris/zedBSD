<!-- awesome-plan project=zedbsd record=ws046p007 -->

# ws046-p007: guest の clang の遅さを解決する（BUG-033）

Phase ID: `ws046-p007`
Parent: [WS046](../ws.md)
Status: uncleared（q411-i01、2026-09-24。修正 2 つは入った。受け入れの 2 項目が既定の 512 MiB で未達）
Queue: q411（q411-i01）。前の試み q405-i01 は uncleared（中断）

## きっかけ

ws046-p004 で guest で expat を build したら、compile が遅すぎた（[BUG-033](../../bugs/BUG-033.md)）。
2026-09-24 ユーザー指示: 「これはコンパイルできたとしても遅すぎます。issueとして扱います。ゲストでclangが遅いことを解決するphaseを書いてください。」

## 目的

guest（amd64、QEMU・KVM）で clang による package の build が実用になる速さにする。原因を測って特定し、kernel（VM・page cache・fault の経路）、
guest の harness（memory の量・disk の種類）のどこで直すかを決めて直す。

## 手順

1. **測る**（host と guest で同じ command）:
   - `clang --version` の cold（起動直後）と warm（2 回目）の壁時計。
   - expat の `lib/xmlparse.c` の compile 1 つ（`-O2`、libtool を通さない clang の直接の呼び出し）の壁時計・user・sys の時間（`time`）。
   - page fault の回数（kernel の counter か計装）、disk の read の量と回数、swap の in/out、空き memory の推移。
   - 同じ測定を guest の memory 512 MiB と 2 GiB、disk を usb-storage と他の interface（virtio-blk か AHCI、zedBSD に driver があるもの）で。
2. **原因ごとに直す**（測った結果で順と範囲を決める）:
   - BUG-027 の fault の経路（`src/kern/vmspace.c` の `fill_file_page` ほか）: 隣の page をまとめて読む（cluster・先読み）、fault ごとの VM の処理の費用を減らす。kernel で、HAL ではない（HAL が要ると分かったら差分ごとに承認を求める）。
   - memory が足りないなら、guest の harness（`plan/tools/guest/guest.py`）の既定の memory を増やす。
   - disk の I/O が律速なら、harness の disk を速い interface に（zedBSD の driver がある範囲で）。
3. 変えるたびに 1 の測定を繰り返して効果を記録する。

## 受け入れ

- guest の `clang --version` が warm で 1 秒以内。
- expat の `xmlparse.c` の compile が、guest で host の 5 倍以内の壁時計（KVM なので CPU はほぼ同じ速さのはず）。
- expat の `make`（依存の追跡あり）が guest で 10 分以内に終わる。
- kernel を変えた場合: build（warning 0）、boot test、関わる回帰（mmap・fault の試験、sh と make の差分試験）。
- 受け入れの数値は、1 の測定の後で根拠とともに見直してよい（見直したら理由を書く）。

## 調べる範囲

1 の測定と、根拠のある原因 2 つまでの修正。それで届かないときは、測った結果と残りの候補を記録して uncleared にし、計画を直す。

## q405-i01（2026-09-24、uncleared: 中断）

2026-09-24 ユーザー指示で WS053（clang/LLVM の LTO を vmunix に、優先度高め）を先にするため、測定の途中で止めた。guest の xmlparse.c の compile の測定はユーザーが止めた。

測ったこと（guest: memory 512 MiB・4 CPU・KVM、disk は usb-storage）:

| 測定 | 結果 |
| --- | --- |
| guest の `clang --version`（3 回） | real 1.48・1.31・1.56 秒（BUG-027 の記録の 11〜15 秒からは速くなっている） |
| guest の空の `main` の `clang -c`（2 回） | real 1.93・2.24 秒 |
| 上の 5 回の子の CPU 時間（`times`） | user 0.16 秒、**sys 6.42 秒**: 時間の大半が kernel の中 |
| host の `xmlparse.c` の `clang -O2 -c`（build/llvm の clang、2 回） | real 1.03 秒（user 1.00、sys 0.03） |

見立て: clang の 1 回ごとに kernel の時間（file の page の fault、`fill_file_page()` の 4 KiB ずつの `file_pread`、private の page の確保と 0 埋め）が秒の単位でかかっている。
`fill_file_page()` は file を裏付けにした page を process ごとの private の page に 1 page ずつ読み、process の間で共有しない（`src/kern/vmspace.c`）。

再開の条件: WS053 の後に、guest の xmlparse.c の compile の測定（real・user・sys）から続ける。

## q411-i01（2026-09-24）

### 測定（修正の前。guest: memory 512 MiB・4 CPU・KVM、usb-storage、kernel は WS053 の full LTO）

| 測定 | 結果 |
| --- | --- |
| guest で expat の `./configure` | 244 秒（p004 は約 6 分） |
| guest の `clang --version`（3 回、`/bin/time`） | real 0.659・0.325・0.198 秒（warm で受け入れの 1 秒以内に入っている） |
| guest の `xmlparse.c` の `clang -DHAVE_EXPAT_CONFIG_H -O2 -c`（2 回） | **real 513・550 秒**（host は 1.03 秒: 約 500 倍） |
| compile の途中の guest の `top` | 空き 342 MiB、swap 0、page-in 0: memory と disk は律速でない。clang の CPU 時間が 1 分 45 秒の時点 |

### 原因: libc の allocator の `free()` が heap の全 block をたどる

compile の途中で gdbstub から 4 つの vCPU の program counter を 40 回とった（`gdb -batch` の `thread apply all bt`）。3 つは `sched_idle`、
clang の vCPU は **40 回とも user 空間の同じ数命令の loop**（`0x1001d4cb2`〜`cbb`、`0x1001d4b97` から呼ばれる）にいた。逆 assemble すると
`src/libc/heap.c` の `heap_allocator_free()` と、そこから呼ばれる `pointer_block()` の loop だった（magic の `0x55534544`・`0x46524545`・`0x42393848`）。

- `pointer_block()`: free と realloc のたびに、pointer の正しさを確かめるために heap の先頭から**全 block の鎖をたどって**探していた。1 回の free が block の数に比例し、
  clang のように何十万の object を作って捨てる program では全体が 2 乗より悪くなる。
- `extend_heap()`: heap が 64 KiB ずつ伸びるたびに、最後の block を探して鎖を全部たどっていた（同じく 2 乗）。
- **BUG-033 の主因は kernel ではなく user 空間の allocator**。q405 の「時間の大半が sys」は短い `clang --version` の起動の話で、compile では当たらない。

host の模型（[tests/heap-model.c](../tests/heap-model.c)、64 KiB ずつ伸びる heap）で再現: 小さい object の free が 1 万個 0.112 秒、2 万個 1.241 秒、4 万個 6.220 秒。

### 修正 1: allocator の pointer の確かめと伸長を定数時間に（`src/libc/heap.c`・`heap.h`）

- `pointer_block()`: 鎖をたどる代わりに、header の magic と、両隣が自分を指し返すこと（前の block の next、次の block の previous、端なら `first`・`last`）を確かめる。
  merge された block は magic を失うので、古い pointer も今までどおり拒む。確かめる問いは鎖をたどるのと同じ。
- `struct heap_allocator` に `last`（heap の最後の block）を足し、split・merge・aligned の分割・伸長で保つ。`extend_heap()` は鎖をたどらずに `last` を使う。
  `heap_allocator_validate()` は鎖の終わりが `last` であることも確かめる。
- 書き直した 3 つの関数（`block_in_heap`・`pointer_block`・`extend_heap`）は規約の全文に合わせた（`style-check.py` の報告 0）。file の他の部分は規約の前の形のまま（範囲外）。

host の検証:

| 検証 | 結果 |
| --- | --- |
| 模型の bench（小さい object の確保と解放） | free 4 万個 6.220 秒 → 0.002 秒、40 万個 0.017 秒 |
| 模型の trace（確保・calloc・aligned・realloc・free・二重の free・block の中の pointer・heap の外の pointer をでたらめに 30 万回、997 回ごとに validate） | 5 つの seed で**修正の前と hash が一致**（返す offset・error の数・validate が全て同じ） |
| `plan/ws004/tests/kernel-heap-lock-test.c`（README の command） | PASS |

guest（512 MiB）: `xmlparse.c` の compile が **513・550 秒 → 5.4〜6.4 秒**（user 1.88 秒・sys 3.88 秒）。

### 修正 2: buffer cache の hash（`src/kern/buf.c`）

修正 1 の後の guest の profile（gdbstub で 150 回）で、memory を 2 GiB にすると**遅くなる**ことが分かった（configure 252 → 405 秒、compile の sys 3.9 → 5.1 秒）。
2 GiB の profile で kernel の leaf の 1 位が `hash_find_locked`（`reference_line` から、kernel の sample の 43/121）。

- hash の bucket が 64 しか無く、cache は memory の 1/16（512 MiB で 8192 の line、2 GiB で 32768）。
- key が `(disk >> 4) ^ block` の下位の bit で、line の先頭の block は line の block 数（512 byte の block で 8）の倍数なので、**使われる bucket は 64 のうち 8 つ**。
  一つの鎖が 512 MiB で約 1000、2 GiB で約 4000 になり、cache の lock を持ったまま cache の読みと file の fault のたびにたどっていた。
- 直し: bucket を 2^13（8192。amd64 で 64 KiB、i386 で 32 KiB の BSS）にし、key を黄金比の乗算の上位の bit にした（line の揃った block も全ての bucket に散る）。

guest の `xmlparse.c` の compile（real、3 回。host は 1.03 秒）:

| memory | 修正 1 の後 | 修正 1+2 の後 |
| --- | --- | --- |
| 512 MiB | 5.4〜6.4 秒（sys 3.9） | 6.5〜6.9 秒（sys 4.3〜4.7）: 変わらない（揺れの内） |
| 2 GiB | wall 6〜9 秒（sys 3.9〜5.1） | **5.05〜5.08 秒**（sys 3.0〜3.2）、host の 4.9 倍 |

`clang --version`（warm）: 512 MiB で 0.89〜1.06 秒、2 GiB で 0.24〜0.30 秒。

### 残りの kernel の時間（修正 1+2 の後、512 MiB の profile）

hash は消えた。kernel の sample の多くは **USB の disk の I/O**（`xhci_irq`・`event_take`・`xhci_urb_enqueue`、`drv_usb_urb_wait_reusable` が
`sched_yield` で完了を待つ **busy wait**）: cache が memory の 1/16（512 MiB で 32 MiB）で LLVM の library（約 160 MiB）が入らず、compile のたびに disk から読み直す。
他に `amd64_percpu_current`（HAL。`rdmsr GS_BASE`）が多くの経路の葉に出る（gdbstub の標本の偏りかもしれない）、process の後始末（`vmspace_destroy` → `vm_page_untrack`）。

### expat の build（修正 1+2 の後）

| memory | configure | make（依存の追跡あり、status） |
| --- | --- | --- |
| 512 MiB（harness の既定） | 204 秒 | **97 秒、status 0**（前は 51 分で `lib/` の 8 file までで止めた） |
| 2 GiB | 150 秒 | **91 秒、status 0** |

`make check`（512 MiB）は status 2: expat の試験の driver が `env bash` を呼び、guest に bash が無い。p008 で扱う（p007 の受け入れの外）。

### 回帰（修正 1+2 の後）

| 検証 | 結果 |
| --- | --- |
| build（既定: `config-amd64`・`intelmac`・`rpi4`・`pcat`・`pc98` の disk image） | 全て status 0。我々の code の warning 0（LLVM・OpenSSH・OpenSSL の外部の source と既存の Noct の 1 件を除く） |
| boot test | amd64（`build/boot-test-amd64-q411/login.png`）・rpi4（`build/boot-test-rpi4-q411`）・pcat（`build/boot-test-pcat-q411`）で login prompt、pc98 は `plan/tools/pc98-boot.py` で login と `uname -a`（`build/boot-test-pc98-q411`） |
| guest の sh の差分試験（全件） | **1388/1425**、落ちた 37 件が WS053 p002 と同じ集合（[evidence/sh-guest.txt](evidence/sh-guest.txt)） |
| guest の make の差分試験 | 91/91（[evidence/make-guest.txt](evidence/make-guest.txt)） |
| serial の対話試験 | 41/41（[evidence/interactive.txt](evidence/interactive.txt)） |
| mmap・fault を使う試験（`SMP-STRESS.ELF`、`smp-resource-stress.c`） | status 0 |
| `POSIX-R2.ELF` | `realtime-signal-capacity` で status 1。**修正の前の image でも同じ**（[BUG-034](../../bugs/BUG-034.md)）。`POSIX-R2-REMAINING.ELF` は compile できない（[BUG-035](../../bugs/BUG-035.md)、試験の code の 16 byte の atomic） |
| kbench（guest、3 回の中央値、WS053 p001 の full LTO と比べて） | syscall 102%・pipe 107%・fork 88%・exec 95%・read 119%・anon fault 103%・file fault 100%。範囲が p001 と重なり、揺れの内（[evidence/guest-tests.txt](evidence/guest-tests.txt)） |
| host の allocator の試験 | 上の表（模型の trace の hash が修正の前と一致、ws004 の試験 PASS） |
| 実機 | 未実施 |

### 受け入れの判定

| 受け入れ | 結果 |
| --- | --- |
| guest の `clang --version` が warm で 1 秒以内 | 2 GiB で 0.24〜0.30 秒（達）。512 MiB で 0.89〜1.06 秒（境目、未達の回がある） |
| `xmlparse.c` の compile が host の 5 倍以内 | 2 GiB で 5.05 秒・4.9 倍（達）。**512 MiB で 6.5〜6.9 秒・6.3〜6.7 倍（未達）** |
| expat の `make` が guest で 10 分以内 | 512 MiB で 97 秒、2 GiB で 91 秒（達） |
| kernel を変えた場合の build・boot・回帰 | 達（上の表） |

調べる範囲（根拠のある原因 2 つまでの修正）を使い切り、harness の既定の 512 MiB で受け入れの 2 項目が届かないので **uncleared**。受け入れの数値は見直さなかった
（見直せば通るが、512 MiB で残る原因が特定できているので、それを直す Phase を立てる方が正しい）。

### 残りの原因（次の Phase の候補、ws046-p009）

1. **file の data の cache が小さい**: buffer cache は memory の 1/16（512 MiB で 32 MiB）で、clang の LLVM の library（約 160 MiB）が入らない。compile のたびに disk から読み直す。
   file を裏付けにした page も process ごとの private の複製（`fill_file_page()`）で、process の間で共有しない。
2. **USB の storage の完了待ちが busy wait**: `drv_usb_urb_wait_reusable()` が `sched_yield()` を回して待つ（profile の kernel の sample の多く）。
3. `amd64_percpu_current()`（HAL、`rdmsr GS_BASE`）が多くの経路の葉に出る。gdbstub の標本の偏りかもしれない。変えるなら HAL の差分ごとの承認が要る。
4. harness の既定の memory（512 MiB）を 2 GiB にすれば、今の kernel でも受け入れの数値に届く（上の表）。1 の直しの代わりにはならない。
