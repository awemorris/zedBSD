# kcrt設計: kernelからlibcを切り離す（ws035-p033）

Phase: `ws035-p033`（Queue q317 / q317-i02）。設計文書であり、ソースは変更していない。
実装は p034（kcrt と heap、vmunix への libc の link をやめる）と p035（include 整理と検査）で行う。
敵対的レビュー（design-reviewer、2026-09-23）とその対応は [phase033/review.md](phase033/review.md)。
本文はレビュー後の改訂版で、レビューで訂正した箇所は review.md の番号を `[R#]` で示す。

根拠として引く行番号は 2026-09-23 の作業ツリー（HEAD `e7a2fb87`、p002 が並行して `include/drivers/` を
移動中）のもの。p002 完了後のパスは [refactor-map.md](refactor-map.md) の p002 の節と、p002 の決定
（`include/drivers/pci/pci-i915.h`・`pci-venus.h`）に従う。


> **2026-09-23 ユーザー決定による上書き（本文より優先）**
>  - Vulkanのヘッダは **UAPIではない**。**libcの一部**として `include/libc/vulkan/` に置く（§7の置き場所を置き換える。
>   同日に一度 `include/vulkan/` と決めたが、さらに置き換えた）。driverは `<libc/vulkan/vulkan.h>` をincludeする
>   （`vulkan.h` の中のincludeは相対パスなので、そのまま解決する）。
> - **kernelがincludeしてよい `libc/` のヘッダは `libc/vulkan/*` だけ**（`<libc/vulkan/vulkan.h>`・`<libc/vulkan/vulkan_core.h>` 等、
>   必要なものを直接includeしてよい）。それ以外のlibcヘッダはkernel・driver・HALからincludeしない。ioctl・errno等のABIはUAPIに分離したままにする（§6.4・§8の方針は維持）。
> - commandの中身がVenus由来のVulkan構造体であるのは意図した設計で、`render/vulkan-codec.inc` と生成器は変えない。
>   `I915_STREAM_MAGIC` の経路は `gpu-i915-test` のための試験用としてUAPIに残す。
> - libcの公開ヘッダは `include/` 直下ではなく **`include/libc/`** に置く（p023）。kernelは `-Iinclude/libc` を使わないので、
>   §14.6のp023との衝突は起きない。sysrootとrootfsへは、`include/libc/` の中身を `/usr/include/` の直下へコピーする。
> - §14の判断事項のうち、compile flagの変更（`-nostdlibinc`、`-fno-builtin`、HALにも適用）、`include/uapi/hosted.h` と
>   fixture 111ファイルへの `-DKERN_UAPI_NATIVE`、明示的な `memcpy`・`memset` の `kern_*` への改名、`locale-record.c` の削除と
>   `hal_memset` 4箇所の置換は承認された。

## 0. 要約

- kernel は今、libc の source 27 ファイル（`libc/libc.mk:26-54` の `ZEDBSD_LIBC_SOURCES`）を vmunix に link し
  （`platform/amd64/vmunix.mk:261-264`）、`-nostdinc -isystem $(sysroot)/usr/include`（同 `:68-69`）で libc の
  公開ヘッダを読んでいる。実際に kernel が呼ぶ libc の関数は heap allocator と 17 個の string/format 関数だけである
  （§1 の実測）。
- kernel 用 C ランタイム **kcrt**（`include/kern/kcrt.h`、`src/kern/kcrt.c`）に `kern_memcpy` 等 16 個と
  `kern_snprintf`/`kern_vsnprintf` を自前で実装し、呼出しを機械的に `kern_*` へ置換する。compiler が暗黙に出す
  `memcpy`・`memset`（実測。§5。HAL の object も `memcpy` を要求する）は同じファイルで標準名の symbol として定義する。
- `libc/heap.c` を複製した `src/kern/heap.c`（`struct kern_heap`、`kern_heap_*`）を kernel 専用に保守し、
  `kern_malloc` 等の API（`include/kern/kmem.h`）は変えない。
- ABI の型と定数（errno、ioctl、types、time、stat、fcntl、limits、mman、wait、statvfs、unistd、un、resource、mount）は
  `include/uapi/` に置き、libc の公開ヘッダはそれを include する側に変える。整数型等は compiler の freestanding
  ヘッダから取る（`-nostdinc -isystem sysroot` → `-nostdlibinc`）。kernel の build は sysroot に依存しなくなる。
- Vulkan の宣言（Khronos 由来、Apache-2.0）は kernel と userland の間の wire 契約なので `include/uapi/vulkan/` に置く。
- 検査は、`-M` による監査（既存 `kernel-include-audit.py` の「0 件」モード）と、vmunix の link 手順に組み込む
  `.d` ベースの常設検査の二段にする。
- host fixture（`plan/*/tests/` 等）を壊さないため、uapi の標準名の宣言は zedBSD target（`__ZEDBSD__`）以外では
  host の C ライブラリに委ねる（§6.4）。これは現状の挙動（kernel source を host で compile すると host の libc ヘッダを
  読む）を保つ規則である。ただし `-Iinclude/libc` で zedBSD の libc ヘッダを読む fixture（111 ファイル、うち 33 は
  `-nostdinc`）には `-DKERN_UAPI_NATIVE` を付ける必要がある [R1]。

## 1. 前提となる事実（実測）

| 事実 | 根拠 |
| --- | --- |
| vmunix は `ZEDBSD_LIBC_SOURCES`（heap、string、string-extra、ctype、fenv、locale、wide、int64、inttypes、strto、stdlib-extra、time-extra、format、stdio、stdio-extra、setjmp、err、libgen、search、random48、random、xsi-crypto、ftw、ndbm、realpath、tempnam、regex 4）を link する | `libc/libc.mk:26-54`、`platform/amd64/vmunix.mk:261-264`（`AMD64_KERNEL_LIBC_OBJS`）、同 `:303-306`（`-fno-builtin -fno-strict-aliasing` 付きの汎用 rule） |
| kernel/HAL の compile は `clang --target=x86_64-unknown-zedbsd --sysroot=… -nostdinc -isystem <sysroot>/usr/include -Iinclude -Isrc -I.`、`-ffreestanding`、`-mgeneral-regs-only`、`-fno-builtin` なし | `Makefile:84-90`（`CC` に triple と sysroot）、`platform/amd64/vmunix.mk:68-80`、baseline log `plan/ws035/phase001/baseline/amd64.log` の `src/kern/entry.c` の行 |
| `-nostdinc` は clang の resource dir（`build/llvm/lib/clang/23/include`）も外す。`-nostdlibinc` は resource dir だけを残す。実際の `--target=x86_64-unknown-zedbsd --sysroot=<p001 の sysroot>` と `-nostdlibinc` で search list は `include src . <resource dir>` の 4 件 | `clang … -nostdlibinc -E -v` の出力（§6.2） |
| kernel が実際に使う libc 関数（amd64 148 object、i915-amd64 210 object の `llvm-nm -u`）: memchr 5、memcmp 31/29、memcpy 90/102、memmove 3、memset 112/143、memset_explicit 1、snprintf 2、strcat 2、strchr 7、strcmp 18/21、strcpy 14、strlen 18、strncmp 7/8、strncpy 6/7、strnlen 2、strrchr 2、strstr 0/1（object 数）。heap は `heap_allocator_init/set_observer/alloc/free/current/peak/largest_free/largest_failed/trace_validate` と `heap_active_set`（`src/kern/entry.c:176,270,313-317,351-356,498`） | `build/ws035-p001/{amd64,i915-amd64}/kern64` の object を `llvm-nm -u` で集計 |
| HAL の object のうち `src/hal/amd64/task.o` と `bsp-pcat/boot.o` は、source に呼出しが無いのに `memcpy` を参照する（構造体代入の compiler 生成）。HAL の source にある `memset(` 呼出し（`src/hal/amd64/bsp-pcat/cons.c:243`）は `#ifdef KERN_CONSOLE_OUTPUT_TEST` の中で、通常 build では compile されない（この macro を定義する config は無い） | `llvm-nm -u build/ws035-p001/amd64/src/hal/**/*.o`、`cons.c:215` [R12] |
| kernel（`src/kern`・`src/drivers`）が読む libc ヘッダ（i915-amd64、object 数）: errno.h 179、string.h 166、locale.h 178、limits.h 165、stddef.h 206、stdint.h 210、stdbool.h 177、features.h 100、sys/types.h 91、time.h 85、sys/time.h 63、sys/ioctl.h 61、sys/stat.h 47、fcntl.h 38、vulkan/{vk_platform,vulkan_core,vulkan_external}.h 20、unistd.h 7、sys/statvfs.h 6、stdarg.h 5、stdio.h 4、signal.h 3、sys/mman.h 3、sys/socket.h 2、sys/wait.h 2、stdlib.h 1、wchar.h 1、poll.h 1、termios.h 1、sys/{mount,resource,select,sysctl,un}.h 各 1。HAL: stdbool 27、stddef 29、stdint 32、errno 1、string 1、locale 2、time 1 | `plan/ws035/phase001/include-audit/i915-amd64.json`（`classes.kernel.libc`、`classes.hal.libc`） |
| `locale.h` の 178 は `string.h:5` が `<locale.h>` を include する連鎖（`strcoll_l` の `locale_t` のため）。`features.h` は `sys/ioctl.h:15`・`sys/time.h:15` の連鎖。`sys/ioctl.h` は uapi の 13 ヘッダが `_IOR` 等のために include している | `include/libc/string.h:5`、`include/libc/sys/ioctl.h:15,30-40`、`include/uapi/{blkid,block,console,fcntl,gpu,gpu-display,gpu-scanout,graphics,input,mountinfo,system,termios,wlan}.h` |
| kernel の allocator は `include/kern/kmem.h` の 4 API。`src/kern/entry.c` が固定 512 KiB heap（`KERNEL_HEAP_SIZE`、`.kernel_heap` section）を `heap_allocator_*` で管理し、2 page 以上は `hal_pmem_alloc` の page-backed allocation。lock は `kernel_heap_lock`（spin、IRQ 無効）。libc の malloc 互換経路のため `__libc_heap_lock/unlock` を override している | `src/kern/entry.c:43-58,98-150,160-297,347-356,411-441` |
| kernel の printf 系は `kern_logf`（`include/kern/klog.h:22-26`、`src/kern/klog.c:106-245`）だけ。対応する書式は `%s %c %u %x %X %d %i %p %%`、`0` flag、10 進幅、`l`/`ll`。`z` と `#` は解釈しない。未対応の変換は `%` と変換文字を出すだけで可変長引数を消費しない（`klog.c:223-226`） | `src/kern/klog.c:101-104,148-164,223-226` |
| kernel の `snprintf` 呼出しは 6 箇所（`"event%u"`、`"usb%u/port%u/device%u/interface%u"`、固定文字列 3、`"Venus virtual display %u"`、`"gpu%u"`） | `src/drivers/generic/input.c:333`、`src/drivers/usb/usb-hid.c:929-959`、`src/drivers/gpu/venus/display.c:747`、`src/drivers/gpu/gpu.c:1072` |
| `kern_logf` を直接呼ぶ箇所の書式は `%u %d %s %x %X %c %p %i %%`、`0` flag と幅、`l`/`ll`。`z`・`#`・精度・`-` は 0 件。i915 display の `%zu`/`%#x`（`dp-sink.c:2127,2157,2242`、`vbt.c:2595`、`panel.c:2063`）は `I915_VBT_LOG`/`I915_DP_LOG` macro（`vbt.h:205-210`、`dp-internal.h:130-135`）に渡るが、この macro は `if (0)` の `drv_i915_vbt_fmtcheck` で compile 時に書式を検査するだけで、引数を書式化せず文字列を `drv_i915_vbt_note(level, fmt)` へ渡す（最終的に `kern_logf("i915: vbt: %s", text)`、`vbt.c:776`）。つまり kernel の書式 engine に `%zu`/`%#x` は届いていない | `grep` の集計、`src/drivers/gpu/i915/display/vbt.h:203-210`、`vbt.c:776` [R4] |
| `src/kern/locale-record.c` は libc の `locale.c`（`libc/locale.c:117-174` が `zed_locale_record_load` を呼ぶ）を kernel に link するためだけに存在する。kernel 側に他の利用者は無い | `grep -rn zed_locale src/kern src/drivers include/kern` が `locale-record.c` 以外に一致しない |
| kernel には `hal_memset` の呼出しが 4 箇所ある（`src/kern/io.c:192,308`、`src/kern/platform/x68k.c:69`、`src/kern/platform/sun4u.c:59`）。`hal_memcpy`/`hal_strlen` の kernel 側呼出しは無い | `grep` [R13] |
| i915 の native Vulkan 実行器は kernel に組み込まれる 8 ファイル（`render/{memory,render-pass,command,pipeline,image,reply,descriptor,instance}.c`）と `render/codec.h`・`render/gfx.h`、kernel 組込みの test（`tests/render/*.c`）が `<vulkan/vulkan_core.h>` を読む。`render/vulkan-codec.inc` は `userland/base/libvulkan/codec.c` から `plan/ws031/handover/tools/gen_vk_server_codec.py` が生成した decoder/encoder の鏡で、`memcpy(` を 21 箇所含む（生成器の `:46,48,50` が emit する） | `platform/amd64/vmunix.mk:132`（`AMD64_I915_SOURCES`）、`src/drivers/gpu/i915/render/vulkan-codec.inc:2-5`、`gen_vk_server_codec.py:46-50` [R8] |
| Vulkan ヘッダの出自は Khronos registry 由来（Apache-2.0、copyright The Khronos Group）で、zedBSD の Noct tool が選択・整形したもの | `include/libc/vulkan/API-PROVENANCE.md:1-12`、`include/libc/vulkan/vulkan_core.h:4-7` |
| HAL が読む libc ヘッダ: `include/hal/types.h:9-11`（stdbool/stddef/stdint）、`src/hal/amd64/int.c:15`・`arm64/int.c:7`・`i386/int.c:14`・`m68k/trap.c:4`・`sparcv9/trap.c:11`（errno.h）、`src/hal/amd64/bsp-pcat/cons.c:22`・`i386/bsp-pc98/cons.c:27`・`i386/bsp-pcat/cons.c:22`・`m68k/bsp-x68k/console.c:7`・`keyboard.c:10`（string.h）、その他 stdint/stddef | `grep` の結果 |
| HAL の C ランタイムは `hal_strlen`・`hal_memset`・`hal_memset16`・`hal_memset32`・`hal_memcpy`・`hal_printf`（「HAL と kernel の初期化段階だけで使う」と注記） | `include/hal/hal.h:40-75`、`src/hal/amd64/lib.c` |
| HAL の object は `-fno-builtin` を足しても `.text` が変わらない（`task.c`、`lib.c`、`bsp-pcat/boot.c` を両 flag で compile し `.text*` の SHA-256 が一致）。`task.c` は `-nostdlibinc`（sysroot ヘッダ無し）でも compile できる | 本 Phase の確認（`build/ws035-p033/`） [R12] |
| host fixture の C ファイル 550 のうち 268 が、host の libc ヘッダ（errno.h 157、fcntl.h 22、unistd.h 12、time.h 10、limits.h 7、sys/stat.h 7、sys/mman.h 5、sys/ioctl.h 4 …）と kernel ヘッダ（`kern/`・`drivers/`・`uapi/`・`hal/`）を同じ翻訳単位で include する。167 の試験 script が `src/kern`・`src/drivers` の source を host で compile する。一方、`-I…/include/libc` を付けて zedBSD の libc ヘッダを読む fixture が 111 ファイルあり、うち 33 は `-nostdinc` も付ける（例 `plan/ws001/tests/directory-fsync-host-test.mk:9`、`plan/ws004/tests/run-intel-ax211-core-test.sh:15`）。`plan/ws018/tests/run-legacy-bootfs-removal-host-test.sh:63` は `src/kern/entry.c` を host で compile する | `plan/*/tests/**`、`src/drivers/gpu/i915/tests/**` の走査 [R1][R15] |
| zedBSD target のうち x86（`x86_64-unknown-zedbsd`、`i386-unknown-zedbsd`）の clang は `__ZEDBSD__ 1` を定義する。`aarch64-unknown-zedbsd`（`platform/arm64/vmunix.mk:6`）は定義しない。kernel・sysroot・userland は `--target=` 付きの `CC` で compile される（`Makefile:84-90`、`toolchain/llvm/sysroot.mk:116`）。host fixture は host の compiler で compile され `__ZEDBSD__` を持たない | `clang --target=… -dM -E` [R7] |
| compiler の暗黙呼出し（§5）: `-ffreestanding` で clang は関数に `"no-builtins"` を付け、loop の idiom 変換（memset/memcpy/memmove/memcmp 化）はしないが、構造体の代入・初期化は `memcpy`/`memset` の libcall に落とす。baseline object で `memcpy` を textual な呼出しなしに参照する object は kernel で amd64 4 個、i915-amd64 9 個（例: `src/kern/acl.c`、`mount.c`、`partition.c`、`intel-ax211-protocol.c`、`gpu.c`、i915 `display/{aux,clock,watermark}.c`、`render/{descriptor,memory}.c`）、HAL で 2 個（上記） | `/tmp/ws035-p033-probe/probe.c` を kernel flags で compile した `llvm-nm -u`、baseline object の集計 |
| `Makefile:470-475` の `ZEDBSD_CHECK_TARGETS` に載る `uapi-abi-layout-check`・`posix-header-check` は rule が無い（`make -n … uapi-abi-layout-check` → `No rule to make target`） | 本 Phase の確認 [R3] |

## 2. 責務と境界

```
                    +------------------ vmunix ------------------+
   HAL (src/hal)    |  kernel (src/kern, src/drivers)             |
   hal_memset 等    |  kcrt.c: kern_mem*/kern_str*/kern_snprintf  |   compiler の freestanding ヘッダ
   hal_printf       |          memcpy, memset（compiler 契約）    |   stdint.h stddef.h stdbool.h stdarg.h limits.h
   （初期化段階）    |  heap.c: kern_heap_*  ← entry.c: kern_malloc |
      │ compiler が  |  klog.c: kern_logf → kern_vsnprintf         |
      │ 生成した     +---------------------------------------------+
      │ memcpy の参照                  |  include/uapi/*.h（ABI の型・定数・構造体、Vulkan 宣言）
      └──→ kcrt.c の memcpy            v
   libc (libc/, userland)  ← include/libc/*.h は uapi を include し、prototype と libc 固有の定義を足す
```

- **kcrt**（`include/kern/kcrt.h`、`src/kern/kcrt.c`）: kernel と driver が使う C ランタイムの最小集合。純関数で、
  lock・allocation・HAL 呼出しを持たず、IRQ 文脈と初期化中を含むどの文脈からも呼べる。HAL の C ランタイム
  （`hal_memcpy` 等）は呼ばない（ユーザー決定）。
- **kernel heap**（`src/kern/heap.c`、`src/kern/heap.h`）: 固定 heap の block 管理だけ。lock は持たず、呼出し側
  （`entry.c`）が `kernel_heap_lock` を保持する（現状どおり）。
- **uapi**（`include/uapi/`）: kernel と userland が共有する ABI の定義。kernel は libc の公開ヘッダを一切読まず、
  ここと compiler の freestanding ヘッダと `include/{kern,hal,drivers,boot}` だけを読む。
- **libc**（`libc/`）: 自分の heap（`libc/heap.c`）と string/format を持ち続ける。公開ヘッダは ABI 定義を uapi から
  include する側になる。kernel には link されない。
- **HAL**: 宣言・実装・責務は変えない。変えるのは `#include` の行（errno.h → uapi/errno.h、string.h → kern/kcrt.h。
  承認済みの範囲）と、HAL/kernel 共通の compile flags（`AMD64_CPPFLAGS`・`AMD64_CFLAGS`。HAL のソースではなく、
  HAL の object の `.text` は変わらない。§14 の判断点 1）。
- **HAL → kcrt の唯一の依存**: HAL の `task.o`・`boot.o` は compiler が生成した `memcpy` を参照する（今は libc の
  `string.o` が供給している）。p034 以降は kcrt.c の標準名 `memcpy` がこれを供給する。これは「HAL は kcrt を
  呼ばない」の唯一の例外で、HAL の source には現れない（compiler 契約）。`KERN_CONSOLE_OUTPUT_TEST` build だけが
  `cons.c:243` の `memset(` を compile し、その宣言を `<kern/kcrt.h>` が与える [R12]。

使い分けの規則（`kcrt.h` の冒頭コメントに書く文面の要点）:

> HAL C ランタイム（`hal_memset`、`hal_memcpy`、`hal_strlen`、`hal_printf`）は HAL 自身と、kernel の初期化段階
> （`kernel_entry()` が heap を初期化するまで）で使う。それ以降の kernel と driver のコードは kcrt を使う。
> kcrt は HAL を呼ばず、HAL は kcrt を呼ばない（compiler が生成する `memcpy`/`memset` の解決を除く）。
> HAL C ランタイムは初期化後もいつでも呼べるが、新しい呼出しは書かない。

kernel 側の既存の `hal_memset` 4 箇所（§1）は、いずれも heap 初期化後の経路（`io_pool_init`、platform の
device 列挙）なので p034 で `kern_memset` に置換する（HAL 側は変えない）[R13]。

## 3. `kcrt.h` の API（設計項目 1）

### 3.1 一覧

すべて `size_t`/`va_list` は compiler の `<stddef.h>`/`<stdarg.h>` から取る。意味は C11 の同名関数と同じで、
違いは「違い」列に限る。

| 関数 | 意味 | 標準関数との違い |
| --- | --- | --- |
| `void *kern_memcpy(void *destination, const void *source, size_t count)` | 重ならない範囲の byte copy | なし |
| `void *kern_memmove(void *destination, const void *source, size_t count)` | 重なってよい copy | なし |
| `void *kern_memset(void *destination, int value, size_t count)` | byte fill | なし |
| `void *kern_memset_explicit(void *destination, int value, size_t count)` | fill を最適化で消させない（鍵・entropy の消去） | C23 `memset_explicit` と同じ。実装は fill の後に compiler barrier |
| `int kern_memcmp(const void *left, const void *right, size_t count)` | byte 比較 | なし |
| `void *kern_memchr(const void *memory, int character, size_t count)` | byte 探索 | なし |
| `size_t kern_strlen(const char *string)` | 終端までの長さ | なし |
| `size_t kern_strnlen(const char *string, size_t maximum)` | 上限付き長さ | なし |
| `int kern_strcmp(const char *left, const char *right)` | 比較 | なし |
| `int kern_strncmp(const char *left, const char *right, size_t count)` | 上限付き比較 | なし |
| `char *kern_strcpy(char *destination, const char *source)` | 終端まで copy | なし（無制限。既存 57 箇所の意味を保つ） |
| `char *kern_strncpy(char *destination, const char *source, size_t count)` | count まで copy、余りは NUL 埋め | なし（C の NUL 埋めを保つ） |
| `char *kern_strcat(char *destination, const char *source)` | 連結 | なし |
| `char *kern_strchr(const char *string, int character)` | 前方探索（NUL も対象） | なし |
| `char *kern_strrchr(const char *string, int character)` | 後方探索 | なし |
| `char *kern_strstr(const char *haystack, const char *needle)` | 部分文字列 | なし |
| `int kern_vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments)` | 書式化（§4 の部分集合） | 対応書式が部分集合。未対応の変換は `%` と変換文字を出し、引数は C の規則で 1 つ消費する |
| `int kern_snprintf(char *buffer, size_t capacity, const char *format, ...)` `__attribute__((format(printf, 3, 4)))` | 同上 | 同上 |
| `void *memcpy(void *, const void *, size_t)`、`void *memset(void *, int, size_t)` | compiler が生成する呼出しの受け皿（§5） | kernel コードから直接呼ばない。HAL の `KERN_CONSOLE_OUTPUT_TEST` build の既存呼出しのために宣言を公開する |

戻り値の規則（snprintf 系）: `capacity` が 0 でなければ常に NUL 終端し、`capacity - 1` byte まで書く。
戻り値は C と同じく「切り詰めなければ書いたはずの長さ」（`int`）。`format == NULL` は 0 を返し `""` を書く
（`kern_logf` の現状 `klog.c:122-123` に合わせた防御）。`%s` に NULL が来たら `(null)`。

含めないもの（理由）: `strlcpy`/`strlcat`（kernel に利用者が無い。安全化は別の改善）、`strdup`（allocator 依存。
`kern_malloc` と組み合わせる利用者が無い）、`ctype`（利用者 0）、`strtol` 系（利用者 0）、`qsort`/`bsearch`（利用者 0）。
必要になった時点で同じ形式で足す。

### 3.2 置換規則（機械的に置換できる範囲）

- 対象: `src/kern/**`、`src/drivers/**`（`src/drivers/gpu/i915-old/**` を除く）、`include/kern/**`、`include/drivers/**`、
  kernel に組み込まれる test source（`platform/amd64/vmunix.mk` の `AMD64_I915_SOURCES`（`I915_TESTS=y` の分を含む）、
  `plan/ws004/tests/pci-msi-qemu.c`）。拡張子は `.c`・`.h`・`.inc`。**除外**: host でしか compile しない harness
  （`src/drivers/gpu/i915/tests/contracts/**`、`src/drivers/gpu/i915/tests/display/host-*.c`・`host-test.h`、
  `plan/*/tests/**`）、HAL（`src/hal/**`、`include/hal/**`）、libc、userland、bootloader。
- 規則: 呼出し構文の token（`\b(memcpy|memmove|memset|memset_explicit|memcmp|memchr|strlen|strnlen|strcmp|strncmp|strcpy|strncpy|strcat|strchr|strrchr|strstr|snprintf|vsnprintf)\s*\(`）
  を `kern_\1(` に置き換える。既に `kern_` が付いているもの、`hal_`・`drv_`・`i915_` 等の接頭辞の一部
  （`\b` で除外）、構造体 member（`->memcpy`、`.memset`）は対象外。コメントや文字列の中の同じ綴りは、置換 script が
  行単位で `/*`・`*`・`//` で始まる行を飛ばし、それ以外は diff で目視する。
- `hal_memset(` の kernel 側 4 箇所（§1）も `kern_memset(` に置き換える（HAL 配下は対象外）[R13]。
- 置換したファイルに `#include <kern/kcrt.h>` を挿入する（最初の `#include` block の末尾。既に含む場合は挿入しない）。
  p034 では `#include <string.h>`・`<stdio.h>` の行は残し（宣言は重複しても同一 prototype なので合法）、p035 で
  同じ script が `<string.h>`/`<stdio.h>` の行を削除する [R5]。
- 生成物 `src/drivers/gpu/i915/render/vulkan-codec.inc`（`memcpy(` 21 箇所）は置換対象だが、生成器
  `plan/ws031/handover/tools/gen_vk_server_codec.py:46-50` が `memcpy(` を emit するので、生成器を `kern_memcpy(`
  を emit するよう直し、再生成した `.inc` と一致することを p034 の受入に含める。`src/drivers/**/*.inc` で
  string 関数を呼ぶのはこのファイルだけで、RTL8822B の `.inc`（ライセンス分離）は触らない [R8]。
- script は `plan/ws035/tests/kcrt-rewrite.py`（決定的、再実行で差分 0）とし、対象ファイル集合と件数
  （呼出し置換、include 挿入、include 削除）を `plan/ws035/phase034/rewrite.json` に記録する。
  規模の見込み: memset 1,566、memcpy 715、memcmp 122、strlen 76、strcmp 75、strcpy 57、strncmp 22、strchr 15、
  strncpy 10、snprintf 6+、memchr 5、strcat 5、strrchr 5、memmove 4、strnlen 3、strstr 3、memset_explicit 1
  （grep の行数。約 2,700 行、約 300 ファイル）。
- host fixture が compile する kernel source は置換後 `kern_*` を呼ぶ。fixture の script を書き換えずに済むよう、
  `kcrt.h` は zedBSD target 以外（host fixture）では `kern_*` を host の libc への `static inline` wrapper として与える
  （§3.3）。

### 3.3 `kcrt.h` の 2 つの顔（native と host fixture）

```c
#include <stddef.h>
#include <stdarg.h>
#include <uapi/hosted.h>            /* KERN_UAPI_HOST_LIBC（§6.4）。p034 で作る */

#if KERN_UAPI_HOST_LIBC && !defined(KERN_KCRT_NATIVE)
/* host fixture: kernel source を host の C ライブラリと compile する。sanitizer の intercept を活かすため
 * 実装は host に委ね、名前だけを与える。 */
#include <string.h>
#include <stdio.h>
static inline void *kern_memcpy(void *destination, const void *source, size_t count) { return memcpy(destination, source, count); }
/* … 16 個すべて同じ形。kern_snprintf/kern_vsnprintf は vsnprintf へ。kern_memset_explicit は memset + barrier … */
#else
/* zedBSD の kernel: 実装は src/kern/kcrt.c */
void *kern_memcpy(void *destination, const void *source, size_t count);
/* … */
void *memcpy(void *destination, const void *source, size_t count);   /* compiler 契約と HAL の test build の呼出しのため */
void *memset(void *destination, int value, size_t count);
#endif
```

- `KERN_KCRT_NATIVE` は kcrt 自身の host 試験（`plan/ws035/tests/kcrt-test.c`）が native 宣言を選ぶための上書き。
  同じ試験は `src/kern/kcrt.c` を `-ffreestanding -fno-builtin -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES` で
  compile する。`-ffreestanding -fno-builtin` が無いと host の cc が kcrt の byte loop を `memcpy`/`memset` の呼出しに
  変換し、glibc を試験することになる。試験は `nm -u kcrt.o` に `memcpy`/`memset`/`memmove`/`memcmp` が無いことも
  確認する。この試験では ASan の memcpy intercept は効かない（kcrt 自身の load/store は instrument される）[R11]。
- `KERN_KCRT_NO_STANDARD_NAMES` は標準名の alias を出さない（host の libc と衝突させない）。
- 選ばなかった案: fixture 167 script に `src/kern/kcrt.c` を足す（編集量が大きく、ASan の memcpy intercept も失う）。
  `kcrt.h` に host の顔を持たせるのは「fixture が kernel source を host の libc で compile する」という既存の前提の
  明示であり、kernel の build には影響しない。

## 4. `kern_snprintf` の書式の範囲（設計項目 2）

kernel が実際に使う書式（§1 の集計）から決める。

| 要素 | 対応 | 根拠 |
| --- | --- | --- |
| 変換 `d i u x X c s p %` | する | `kern_logf` の現状（`klog.c:101-104`）と一致。`%p` は `0x` + `sizeof(uintptr_t)*2` 桁の 0 埋め（現状どおり） |
| flag `0` | する | `%08x` 1,093 箇所など |
| flag `#` | する（`x`/`X` に `0x`/`0X`、値 0 では付けない） | `kern_logf` に届く利用は今は 0（§1。i915 の `%#x` は macro が書式化しない）。`-Wformat` を通る書式を将来 `kern_logf` に渡せるようにする予防で、費用は小さい [R4] |
| 幅（10 進） | する | `%2u`、`%016llx` など |
| 長さ `l`、`ll` | する | 現状どおり |
| 長さ `z` | する（`size_t`/`ssize_t`） | 同上の予防（`size_t` の引数を `%zu` で `-Wformat` 適合のまま渡せる）。現状の `kern_logf` 利用に `%zu` は無い [R4] |
| 長さ `h`、`hh` | 受理して `int` として扱う | clang の `-Wformat` が通る組合せを潰さないため（現状の利用は 0） |
| 精度 `.N`/`.*`、幅 `*`、flag `-`/`+`/空白、`o`、`n`、`e/f/g`、`j`/`t` | 変換はしない。未対応の変換は `%` と変換文字をそのまま出す。**ただし引数は C の規則で消費する**（長さ無し/`h`/`hh` → `int`、`l` → `long`、`ll` → `long long`、`z` → `size_t`、`s`/`p`/`n` → pointer、`.*`/`*` → `int`）ので、以降の引数はずれない。`-Wformat` はこれらを合法として通すので、engine 側で守る [R10] | 利用 0。kernel は浮動小数点を使わない（`-mgeneral-regs-only`）。`f/e/g` は `double` を消費する（`va_arg(ap, double)` は `-mgeneral-regs-only` でも compile できるが SSE を使うため、`f/e/g` は消費せず `%` と文字を出したうえで kern_logf の呼出し元を受入 script で 0 件に保つ）|

`kern_logf`（`src/kern/klog.c`）は自前の書式 engine（`emit_char/emit_text/emit_number`）を捨て、
`kern_vsnprintf` で 512 byte の stack buffer に描画して `kern_log_write` する形に変える。現状の利用範囲では出力は
同一になる。HAL の `hal_printf` は HAL の所有物なので触らない。i915 の `drv_i915_vbt_fmtcheck` 等
（`vbt.h:540`、compile 時の書式検査だけの関数）はそのまま。

受入で、`kern_logf`・`kern_snprintf`・`drv_i915_vbt_fmtcheck`/`drv_i915_lcd_fmtcheck` 等の呼出しの文字列リテラルを
§1 の正規表現で走査し、許可集合（上表の「する」）以外の変換が 0 件であることを確かめる script
（`plan/ws035/tests/kcrt-format-scan.py`）を p034 に加える [R10]。

kcrt 側は `kern_vsnprintf` に engine を持ち、`kern_snprintf` はその薄い wrapper。engine は stack だけを使い
（数字 buffer 24 byte 程度）、再入可能で IRQ 文脈から呼べる。

選ばなかった案: `libc/format.c`（602 行、`wchar.h`・`stdio.h`・locale 依存）の複製。kernel の書式は上の部分集合で
足り、複製は保守対象を増やすだけなので `klog.c` の engine を kcrt へ移して拡張する。

## 5. compiler が暗黙に出す呼出しの扱い（設計項目 3）

実測（§1、`/tmp/ws035-p033-probe/probe.c`、kernel と同じ flags）:

| flags | 未定義 symbol |
| --- | --- |
| `-ffreestanding`（kernel の現状） | `memcpy memset` |
| `-ffreestanding -fno-builtin` | `memcpy memset` |
| （なし） | `memcpy memmove memset`（loop idiom が memmove 化される） |

- clang は `-ffreestanding` で関数に `"no-builtins"` 属性を付け（IR で確認）、loop の memset/memcpy/memcmp 化は
  しない。一方、構造体の代入・`= {0}` 初期化・配列 copy は LLVM が `llvm.memcpy`/`llvm.memset` intrinsic に落とし、
  inline 展開の閾値を超えると `memcpy`/`memset` の libcall になる。これは `-fno-builtin` では止まらない（言語仕様上
  freestanding でもこの 4 つは要求されうる）。HAL の `task.o`・`boot.o` も同じ理由で `memcpy` を参照する。
- 定義を置く名前: p034 で libc object を link から外し、link error に現れた名前を `src/kern/kcrt.c` に定義する
  （ユーザー決定）。予測は `memcpy` と `memset` の 2 つ。実装は `kern_memcpy`/`kern_memset` の alias
  （`__attribute__((alias("kern_memcpy")))`。同一翻訳単位で probe 済み: `-fno-builtin` 下で未定義 symbol 0、
  自己再帰なし）で、コードの重複を作らない。`memmove`/`memcmp` は現れなければ定義しないが、
  `kern_memmove`/`kern_memcmp` は存在するので、将来 link error が出たときの追加は alias 1 行である。
- `AMD64_CFLAGS` に `-fno-builtin` を明示して足す（clang では `-ffreestanding` が含意するが、意図を flags に残し、
  kcrt.c 自身の byte loop が memcpy に変換されて自己再帰する古典的事故を二重に防ぐ）。HAL の object はこの flag で
  変わらない（§1）。
- 確かめ方: (1) p034 の link（`$(BUILD)/vmunix`）が通ること自体が検査。(2) `llvm-nm -u $(BUILD)/kern64/src/kern/kcrt.o`
  が空であること（kcrt が何も外部に依存しない）。(3) `llvm-nm --defined-only kcrt.o` に `memcpy`・`memset` が
  あること。(4) 標準名を参照する object の一覧（kernel と HAL の全 object を `llvm-nm -u` し、source に
  `kern_*` 以外の textual な呼出しが無いもの）を `plan/ws035/phase034/implicit-calls.txt` に記録する [R12]。
- host fixture: host の libc に任せる（§3.3）。kcrt.c の host 試験は `-DKERN_KCRT_NO_STANDARD_NAMES` で alias を外す。

## 6. 型と定数の出どころ、compile flags（設計項目 5）

### 6.1 出どころの規則

| 種類 | 出どころ | 例 |
| --- | --- | --- |
| 整数型・`size_t`・`NULL`・`offsetof`・`bool`・`va_list`・C の限界値 | compiler の freestanding ヘッダ（`build/llvm/lib/clang/23/include/{stdint,stddef,stdbool,stdarg,limits}.h`。`__STDC_HOSTED__ 0` なので `#include_next` せず自前定義になる。実測済み） | `#include <stdint.h>` の綴りは変えない |
| errno、ioctl の `_IO*`、POSIX 型、time、stat、fcntl の flag、limits の system 値、mman、wait、statvfs、unistd の access/seek、`sockaddr_un`、rlimit/rusage、mount | `include/uapi/*.h`（既存 44 ファイルに 13 を足し、3 を拡張する。§8 の表） | `#include <uapi/errno.h>` |
| kernel 内部の型 | `include/kern/*.h` | 変更なし |
| C ランタイム | `include/kern/kcrt.h` | `#include <kern/kcrt.h>` |
| Vulkan 宣言 | `include/uapi/vulkan/` | §7 |

compiler ヘッダとの差: libc の `stdint.h`（`include/libc/stdint.h:104-118`）は `UINT32_C` 等を suffix macro で作り、
clang のものは `__UINT32_C` builtin を使うが、どちらも同じ値になる。`max_align_t` は libc が `union{long long; double; void*}`
（8 byte 揃え）、clang は `long double` を含む 16 byte 揃えになる。kernel に `max_align_t` の利用者が無いことを p035 で
grep して確かめる（あれば整列の変化を評価する）。`MB_LEN_MAX` は libc 4／clang 1 だが kernel の利用は 0。
`SSIZE_MAX`・`PATH_MAX`・`NAME_MAX`・`ARG_MAX`・`HOST_NAME_MAX`・`GETENTROPY_MAX` は compiler の `limits.h` に無いので
uapi へ移す（§8）。

### 6.2 kernel の compile flags（p035）

`platform/amd64/vmunix.mk:68-80` を次にする（i915 も同じ変数を使う）。

```make
AMD64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON -DKERN_USER_ABI_LP64 …（他は現状どおり）
AMD64_CFLAGS := …（現状）… -fno-builtin
```

- `-nostdinc` → `-nostdlibinc`: clang の resource dir（freestanding ヘッダ）だけを残し、sysroot の `usr/include` を外す。
  実際の `CC`（`--target=x86_64-unknown-zedbsd --sysroot=<p001 の sysroot>`）で search list が
  `include src . <resource dir>` になることを確認済み（§1）。`CC` に付く `--sysroot=`（`Makefile:88-89`）は残るが
  探索されない。
- `-isystem` が無くなるので `-MMD` の `.d` に kernel が読んだ tree 内の全ヘッダが載る（resource dir のヘッダは system
  扱いのまま載らない）。これが §9 の常設検査の入力になる。
- `$(AMD64_VMUNIX_OBJS): $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete`（`vmunix.mk:271`）を外す。kernel は
  sysroot を読まない。sysroot は userland のために引き続き build される。
- `AMD64_KERNEL_LIBC_OBJS`（`vmunix.mk:261-264`）と `AMD64_VMUNIX_OBJS` からの参照を p034 で外す。
  `$(BUILD)/kern64/%.o: %.c` の汎用 rule（`:303-306`）は `plan/ws004/tests/pci-msi-qemu.c` のために残す。
- `AMD64_KERNEL_SOURCES` に `src/kern/kcrt.c`、`src/kern/heap.c` を足し、`src/kern/locale-record.c` を外す。
- GCC の platform（sparcv9、m68k）には `-nostdlibinc` が無い。`-nostdinc -isystem $(shell $(CC) -print-file-name=include)`
  が対応形だが、amd64 以外の build は WS036 の範囲（今は壊れてよい）。

### 6.3 kernel の build は sysroot から独立する

`-isystem sysroot` と libc object の両方が消えるので、`make vmunix` は sysroot の生成を待たない。build 時間と、
libc の変更が kernel を再 compile する連鎖（`vmunix.mk:270` のコメント「A refreshed ABI must rebuild consumers」）が
無くなる。uapi の変更は `.d` 経由で正しく再 build される。

### 6.4 host fixture との共存: `include/uapi/hosted.h`

268 の fixture ファイルが host の `<errno.h>`・`<fcntl.h>`・`<sys/stat.h>` 等と kernel ヘッダを同時に include する
（§1）。uapi が `EINVAL`・`O_CREAT`・`struct stat`・`PATH_MAX` を無条件に定義すると、glibc の定義と衝突して
（macro の再定義は `-Werror` で error、構造体は再定義 error）これらの fixture が全滅する。現状は「kernel source を
host で compile すると host の libc ヘッダを読む」ことで整合している（kernel の `<errno.h>` が host では glibc の
errno.h を指す）。この挙動を保つ。

```c
/* include/uapi/hosted.h — これらのヘッダが zedBSD の ABI を定義するか、host の C ライブラリに委ねるか。
 * zedBSD の x86 build（kernel、libc、program）は zedBSD target で compile され __ZEDBSD__ を持つ。
 * host fixture は host の compiler と C ライブラリで kernel source を compile し、__ZEDBSD__ を持たない。
 * それ以外の zedBSD build（aarch64 の clang、GCC の platform、zedBSD の libc ヘッダを -I で読む fixture）は
 * KERN_UAPI_NATIVE を定義する。 */
#if defined(__ZEDBSD__) || defined(KERN_UAPI_NATIVE)
#define KERN_UAPI_HOST_LIBC 0
#else
#define KERN_UAPI_HOST_LIBC 1
#endif
```

標準名を持つ uapi ヘッダは、標準名の部分だけをこの switch で囲む。zedBSD 固有名（`KERN_*`、`uapi_ptr_t`、
`flock_record`、`tid_t`、`reclen_t`、`_IO*` を使う ioctl 番号など）は無条件に定義する。

```c
/* include/uapi/errno.h */
#include <uapi/hosted.h>
#if KERN_UAPI_HOST_LIBC
#include <errno.h>           /* host fixture: host の番号（今までどおり） */
#ifdef LIBC_ERRNO_H          /* 委ねた先が zedBSD の libc だった: 値が循環で消える。-DKERN_UAPI_NATIVE が要る */
#error "zedBSD libc headers are on the include path of a hosted build; define KERN_UAPI_NATIVE"
#endif
#else
#define EDOM 1
…（include/libc/errno.h:18-97 の値をそのまま移す）
#endif
```

- `__ZEDBSD__` を定義するのは x86 の patched clang（`x86_64-unknown-zedbsd`、`i386-unknown-zedbsd`）だけである。
  kernel（`Makefile:84-90` の `CC`）、sysroot の libc（`sysroot.mk:116` の `--target`）、userland、cross build の
  package はこれで native になる。host fixture では定義されない。**aarch64（`platform/arm64/vmunix.mk:6`）の clang は
  `__ZEDBSD__` を定義しない**ので、arm64 では kernel だけでなく libc・userland の全 compile に `-DKERN_UAPI_NATIVE`
  を与えるか、clang patch を aarch64 に広げる必要がある。GCC の platform（sparcv9、m68k）も同じ。いずれも
  WS036 の範囲（今は壊れてよい）[R7]。
- **`-Iinclude/libc` を付ける fixture（111 ファイル、うち 33 は `-nostdinc`）は委任先が zedBSD の libc になり、
  `include/libc/errno.h` → `<uapi/errno.h>`（guard 済みで空）の循環で `E*` が未定義になる。** これらは zedBSD の
  libc ヘッダで compile する fixture なので `-DKERN_UAPI_NATIVE` を付けるのが正しい。上の `#error`（`LIBC_ERRNO_H` は
  `include/libc/errno.h:8` の guard）が、付け忘れを未定義 macro の連鎖ではなく 1 行の error にする。同じ検出を
  他の委任ヘッダにも入れる（各 libc ヘッダの guard 名: `LIBC_FCNTL_H`、`LIBC_SYS_STAT_H`、`KERN_TIME_H` 等）。
  fixture の一覧と `-D` の追加は p035 の作業（`plan/ws035/phase035/native-fixtures.txt`、sed による機械的な追加）[R1]。
- `KERN_UAPI_NATIVE` は、host で zedBSD の ABI 値を検査したい試験（§9 の台帳検査など）のための上書きでもある。
- 委ねるときの `#include <errno.h>` は、p023 より前は host の glibc に解決する（`include/libc/` を `-I` に持たない
  fixture の場合。`-idirafter include/libc` を付ける fixture（例 `plan/ws029/tests/run-i915-host-tests.sh:16`）でも
  system dir が先）。**p023 で `include/errno.h` が現れると、fixture の `-Iinclude` がそれを先に見つけてこの規則が
  破れる。同じ理由で kernel 自身の build も壊れる（§14.6）。** p034/p035 の受入は p023 より前に行うので影響しない。

選ばなかった案:
- fixture 167 script すべてに `-DKERN_HOST_FIXTURE` を足す（正の検出）: 編集量が大きく、`__ZEDBSD__` で同じ区別が
  できる。ただし `-Iinclude/libc` の 111 ファイルには `-DKERN_UAPI_NATIVE` が要る（上記）。
- kernel 側の名前を `KERN_EINVAL` 等に改名: 300 ファイル・数千箇所の改名になり、標準名を uapi に置く方針
  （ws.md 未決事項 6）とも合わない。
- `__STDC_HOSTED__` で判定: cross build の package は hosted なので誤判定する。

## 7. Vulkan ヘッダの扱い（設計項目 6）

決定: **`include/uapi/vulkan/` に置く。**

- 移すもの: `include/libc/vulkan/{vk_platform.h,vulkan_core.h,vulkan_external.h,API-PROVENANCE.md,LICENSE-API}`
  → `include/uapi/vulkan/`（`git mv`、内容は変えない。相対 include `"vk_platform.h"`・`"vulkan_external.h"`
  （`vulkan_core.h:21,5679`）はそのまま動く）。
- 残すもの: `include/libc/vulkan/vulkan.h`（標準の入口。`<uapi/vulkan/vulkan_core.h>` を include するよう 1 行変える）、
  `vulkan_wayland.h`（userland 専用。`:9` の `"vulkan_core.h"` は下の wrapper 経由で解決する）。
  `include/libc/vulkan/vulkan_core.h` と `vulkan_external.h` は `#include <uapi/vulkan/…>` だけの互換ファイルとして
  残す（`<vulkan/vulkan_core.h>` は Khronos の標準の綴りで、userland の program が使いうる）。
- kernel 側: i915 の 8+2+5 ファイルの `#include <vulkan/vulkan_core.h>` を `<uapi/vulkan/vulkan_core.h>` に変える。
- tool と文書: `userland/base/libvulkan/tools/maintain-dispatch.noct:20,22` の `include/libc/vulkan/vulkan_external.h` の
  パスと banner、その生成物 `userland/base/libvulkan/dispatch-table.inc:9`、`userland/base/libvulkan/README.md:6,65`、
  `userland/base/vkdemo/README.md:21`、`API-PROVENANCE.md` の生成先の記述を新パスに直す。生成物
  （`dispatch-table.inc`）を再生成して一致を確かめる（ws030 の再生成試験の手順）[R16]。
- sysroot: `toolchain/llvm/sysroot.mk:75` は `include/uapi` を丸ごと sysroot に入れるので、`usr/include/uapi/vulkan/`
  （`LICENSE-API`・`API-PROVENANCE.md` を含む。今の `usr/include/vulkan/` と同じ扱い）が加わる。manifest の変化は
  p035 の受入で記録する。

理由:
1. kernel の i915 render は Vulkan の構造体と enum を **wire format** として decode する（`render/vulkan-codec.inc` は
   `userland/base/libvulkan/codec.c` の鏡）。同じ宣言を kernel と userland が読まなければ両者は drift する。
   これは uapi の定義そのものである。
2. p035 の時点（p023 より前）では `include/libc/` が kernel の include path から消えるので、そこにある限り
   kernel は読めない。p023 後の `include/vulkan/` は libc ヘッダの領域で、kernel が読むと「0 件」の検査に恒久の
   例外を作ることになる。
3. 3 ファイルは `<stddef.h>`・`<stdint.h>` 以外に依存しない（`vk_platform.h:16,20`）ので uapi の条件を満たす。

選ばなかった案: (a) 例外として `vulkan/` を kernel の許可リストに載せる（理由 2）。(b) kernel 専用の複製
`include/drivers/gpu/vulkan-core.h`（5,685 行の複製と drift）。(c) 例外的な `-I` の追加（kernel の include 規則に
穴を開ける）。

ライセンス: Apache-2.0 の Khronos 由来宣言を `include/uapi/` に置くことは、既に sysroot の `usr/include/vulkan/` に
置いているのと同じ露出である。ヘッダの著作権表示（`vulkan_core.h:3-7`）、`LICENSE-API`、`API-PROVENANCE.md` は
一緒に移し、変更しない。§13 も参照。

## 8. どの kernel ファイルがなぜ libc ヘッダを読んでいるか、置換先（設計項目 7）

分類は「直接 include」と「連鎖（どのヘッダ経由か）」。件数は object 数（i915-amd64）または直接 include の
ファイル数。置換先は p035 の作業表になる。**移す要素の一覧はこの表を出発点とし、p035 の着手時に
`plan/ws035/tests/uapi-move-ledger.py`（libc ヘッダの `#define`/typedef/struct 名を抽出して kernel source を grep し、
`include/uapi` に無いものを列挙する）で機械的に補完する** [R6]。

| libc ヘッダ | 直接 include するファイル | 連鎖の経路 | kernel が使う要素 | 置換先 |
| --- | --- | --- | --- | --- |
| `errno.h`（179） | `src/kern` 71、`src/drivers` 約 150、`include/kern/{file-backing.h:10,io-context.h:15}`、`include/drivers/pci/pci-nvme-protocol.h`、HAL 5 | 上記 2 kern ヘッダ経由 | `E*` 定数のみ（`errno` 変数は不使用） | 新 `include/uapi/errno.h`（値を `include/libc/errno.h:18-97` から移す。`include/libc/errno.h` は `<uapi/errno.h>` を include し `errno` macro と `__libc_errno_location` だけ残す） |
| `string.h`（166） | `src/kern` 60、`src/drivers` 約 120、`src/drivers/gpu/i915/{display/internal.h,render/codec.h}`、HAL 5 | — | 17 関数（§1） | `<kern/kcrt.h>`（呼出しは `kern_*`。HAL の test build の `memset` は kcrt の標準名） |
| `locale.h`（178） | `src/kern/locale-record.c:14` | `string.h:5`（`locale_t`）、`time.h:11`、`stdio.h:11`、`stdlib.h:5` | 無し（連鎖だけ） | string.h を外せば消える。`locale-record.c` は削除（§1: libc の locale.c のためだけの存在） |
| `features.h`（100） | — | `sys/ioctl.h:15`、`sys/time.h:15`、`sys/resource.h`、`unistd.h`、`signal.h` | `__ZEDBSD_LEGACY_VISIBLE`（prototype の可視性。kernel は不使用） | 連鎖元を uapi にすれば消える |
| `stdint.h`（210）、`stddef.h`（206）、`stdbool.h`（177）、`stdarg.h`（5）、`limits.h`（165） | 多数（`include/hal/types.h:9-11`、`include/kern/atomic.h:12-13` 等） | `sys/types.h:11-12` 等 | 整数型、`size_t`、`bool`、`va_list`、`INT_MAX`/`UINT_MAX`/`CHAR_BIT` | 綴りを変えず compiler の freestanding ヘッダに解決させる（§6.2）。`NAME_MAX`/`PATH_MAX`（16 ファイル: `include/kern/{exec,namei,mount,file}.h`、`src/kern/{file,mount,exec,namecache,inode,namei,tmpfs,syscall}.c`、`net/unix-socket.c`、`src/drivers/fs/{overlayfs,ufs,fat}.c`）、`SSIZE_MAX`（`syscall.c:4417`）、`GETENTROPY_MAX`（`syscall.c:5180,5188`）は新 `include/uapi/limits.h`（`ARG_MAX`・`HOST_NAME_MAX`・`_POSIX_*` の system 限界も一緒に移す。`BC_*`・`LINE_MAX` 等の utility 限界は libc に残す）[R6] |
| `sys/types.h`（91） | `include/kern/{cdev,cred,disk,inode,posix-acl,process,quota,signal,thread,tty,uaccess,vm-object,vmspace}.h`、`include/kern/net/socket.h:22`、`include/uapi/{process.h:13,socket.h:23}` | `sys/stat.h:15`、`fcntl.h:63`、`sys/time.h:16` 等 | `ssize_t off_t dev_t ino_t mode_t nlink_t uid_t gid_t pid_t id_t tid_t useconds_t suseconds_t reclen_t blkcnt_t blksize_t` | `include/uapi/types.h` に typedef を移す（`KERN_USER_ABI_LP64` の分岐は既に同じ形で uapi/types.h にある）。`include/libc/sys/types.h` は `<uapi/types.h>` を include し `caddr_t` だけ残す |
| `time.h`（85）、`sys/time.h`（63） | `include/kern/{clock.h:17,process-timer.h:13}`、`src/kern/syscall.c`、`include/uapi/{input.h:17,socket.h:22}` | `sys/stat.h:16-17`、`sys/select.h`、`poll.h`、`signal.h`、`sys/resource.h:17` | `time_t clockid_t timer_t`、`struct timespec/timeval/itimerspec/itimerval`、`CLOCK_MONOTONIC/REALTIME`、`TIMER_ABSTIME`、`UTIME_NOW/OMIT`、`ITIMER_*` | 新 `include/uapi/time.h`（`struct tm`、`clock_t`、prototype、`timeradd` 系 macro は libc に残す） |
| `sys/stat.h`（47） | `include/kern/{exec.h:18,inode.h:17}`、`src/kern/{swap,syscall}.c` | — | `S_IF*`/`S_IS*`、mode bit、`struct stat`（39 箇所）、`st_atime/st_mtime/st_ctime` macro（8/8/7 箇所）、layout の `_Static_assert` | 新 `include/uapi/stat.h`（`_Static_assert` と `st_*time` macro も移す。prototype は libc）[R6] |
| `fcntl.h`（38） | `include/kern/file.h:24` と 20 の `.c` | — | `O_*`（49+33+10…）、`AT_*`、`F_*`、`FD_*` | 既存 `include/uapi/fcntl.h` を拡張（`struct flock` は kernel 不使用なので libc に残す） |
| `sys/ioctl.h`（61） | `include/uapi/*` 13 ファイル | uapi 経由で kernel 全体 | `_IO/_IOR/_IOW/_IOWR`（`KERN_IOC*`） | 新 `include/uapi/ioctl.h`（`include/libc/sys/ioctl.h:30-40` の macro を移す）。13 の uapi ヘッダと `include/libc/sys/ioctl.h` が include |
| `stdio.h`（4） | `src/drivers/generic/input.c`、`src/drivers/gpu/gpu.c`、`src/drivers/gpu/venus/display.c`、`src/drivers/usb/usb-hid.c`、`src/kern/record-lock.c` | — | `snprintf`（4 ファイル）、`SEEK_SET/CUR/END`（`file.h`、`file.c`、`record-lock.c` の 13 箇所。`stdio.h:24-26` 由来） | `snprintf` → `<kern/kcrt.h>`。`SEEK_*` → 新 `include/uapi/unistd.h` |
| `unistd.h`（7） | `src/kern/{acl,cred,exec,file,namei,syscall}.c`、`net/unix-socket.c` | — | `R_OK W_OK X_OK F_OK`、`SEEK_DATA/HOLE`（`include/kern/file.h:42-46` は `#ifndef` で自前定義も持つ） | 新 `include/uapi/unistd.h`（`F_OK…R_OK`、`SEEK_*`、`STD*_FILENO`。`include/libc/{unistd,stdio}.h` が include） |
| `sys/mman.h`（3） | `src/kern/{syscall,vm,vmspace}.c` | — | `PROT_*`、`MAP_*`（`MAP_ANONYMOUS`/`MAP_FIXED_NOREPLACE` を含む）、`MS_SYNC/MS_ASYNC/MS_INVALIDATE`（12/9 箇所）、`MADV_*` | 新 `include/uapi/mman.h` [R6] |
| `sys/wait.h`（2） | `src/kern/{process,syscall}.c` | `signal.h`（3 の内訳） | `W*` macro、`WNOHANG/WUNTRACED/WCONTINUED`、`idtype_t`/`P_*` | 新 `include/uapi/wait.h` |
| `sys/statvfs.h`（6） | `src/kern/{devfs,mount,syscall,tmpfs}.c`、`src/drivers/fs/{fat,ufs}.c` | — | `struct statvfs`、`ST_*`、`fsblkcnt_t` | 新 `include/uapi/statvfs.h` |
| `sys/resource.h`（1） | `src/kern/syscall.c` | — | `struct rlimit`（12）、`struct rusage`（2）、`rlim_t`、`PRIO_PROCESS/PGRP/USER`、`RUSAGE_SELF/CHILDREN`（`syscall.c:8050-8112` 他） | 既存 `include/uapi/resource.h` を拡張（`RLIMIT_*` は既にある。`struct rusage` は `uapi/time.h` の `timeval` を使う）[R6] |
| `sys/mount.h`（1） | `src/kern/syscall.c:3829-3870` | — | `MNT_RDONLY`/`MNT_NOSUID`（`:3846`）、`KERN_MOUNT_ARGS_VERSION`、`KERN_MOUNT_FSPEC_MAX`、`struct mount_args`（`:3829,3858`） | 新 `include/uapi/mount.h`（`include/libc/sys/mount.h:18-29` から移す。`MNT_FORCE` は `uapi/unmount.h` のまま）[R2] |
| `sys/un.h`（1） | `src/kern/net/unix-socket.c` | — | `struct sockaddr_un`（8）、`UNIX_PATH_MAX` | 新 `include/uapi/un.h` |
| `termios.h`、`poll.h`（各 1） | `src/kern/tty.c` | — | `struct termios`/`winsize`、`struct pollfd`/`POLLIN`（すべて uapi に既存） | `<uapi/termios.h>`、`<uapi/poll.h>` に変えるだけ |
| `sys/socket.h`（2）、`sys/select.h`、`sys/sysctl.h`（各 1） | `src/kern/syscall.c`（socket は `net/unix-socket.c` も） | — | uapi の同名ヘッダの内容（`MSG_DONTWAIT`/`MSG_NOSIGNAL` は `uapi/socket.h:112-113`） | include 行を削除（compile で確認） |
| `stdlib.h`（1）、`wchar.h`（1） | `src/kern/syscall.c:84` | `stdlib.h:13` → `wchar.h` | 利用が見つからない（`abs`/`strtol`/`EXIT_*` 等 0） | include 行を削除（compile で確認） |
| `signal.h`（3） | — | `sys/wait.h:16`、`sys/select.h:16`、`poll.h:16` | 無し（`uapi/signal.h` を直接使っている） | 連鎖元を uapi にすれば消える |
| `vulkan/*.h`（20） | i915 render/tests（§7） | — | Vulkan 型・enum | `<uapi/vulkan/vulkan_core.h>` |
| libc 内部 `libc/heap.h`、`libc/locale-db.h`、`locale-format.h` | `src/kern/entry.c:22`、`src/kern/locale-record.c:12` | — | heap API、locale 表 | `src/kern/heap.h`（§8.1）、`locale-record.c` 削除 |

新規 uapi ヘッダは 13（errno、ioctl、time、stat、limits、unistd、mman、wait、statvfs、un、mount、hosted、vulkan/）、
拡張は 3（types、fcntl、resource）。

HAL（承認済みの `#include` パス変更に該当）:

| ファイル | 行 | 変更 |
| --- | --- | --- |
| `src/hal/amd64/int.c` | 15 | `<errno.h>` → `<uapi/errno.h>` |
| `src/hal/amd64/bsp-pcat/cons.c` | 22 | `<string.h>` → `<kern/kcrt.h>`（`KERN_CONSOLE_OUTPUT_TEST` build の `memset` 呼出しのため。呼出しは変えない） |
| `src/hal/arm64/int.c`、`i386/int.c`、`m68k/trap.c`、`sparcv9/trap.c` | 7、14、4、11 | `<errno.h>` → `<uapi/errno.h>`（amd64 以外。build は WS036） |
| `src/hal/i386/bsp-pc98/cons.c`、`i386/bsp-pcat/cons.c`、`m68k/bsp-x68k/console.c`、`keyboard.c` | 27、22、7、10 | `<string.h>` → `<kern/kcrt.h>`（同上） |
| `include/hal/types.h` | 9-11 | 変更なし（compiler ヘッダに解決） |

uapi 側の連鎖の修正（p035）: `include/uapi/{blkid,block,console,fcntl,gpu,gpu-display,gpu-scanout,graphics,input,mountinfo,system,termios,wlan}.h` の
`<sys/ioctl.h>` → `<uapi/ioctl.h>`、`include/uapi/{input.h:17,socket.h:22}` の `<sys/time.h>` → `<uapi/time.h>`、
`include/uapi/{process.h:13,socket.h:23}` の `<sys/types.h>` → `<uapi/types.h>`。これで uapi は uapi・compiler
ヘッダ以外を読まなくなる（userland から見ても閉じる）。

### 8.1 `src/kern/heap.c` の構成（設計項目 4）

複製元 `libc/heap.c`（904 行）との対応。`libc/heap.c` は変えない。

| `libc/heap.c` | `src/kern/heap.c` | 扱い |
| --- | --- | --- |
| `struct heap_allocator`（`libc/heap.h:32-49`） | `struct kern_heap` | `original_base/original_size`（reset 用）、`fail_after/successful_allocations`（失敗注入）、`grow/grow_context`（拡張）を落とす。`begin end first free_list current_bytes peak_bytes largest_failed_allocation errors observer observer_context` を残す（`errors` は `entry.c:498` が読む） |
| `struct heap_block`、`HEAP_ALIGNMENT 16`、magic/state | 同名 | そのまま（16 byte 揃えの理由コメント `heap.c:15-25` も保つ） |
| `heap_allocator_init` | `kern_heap_init(struct kern_heap *, void *base, size_t size)` | `memset` → `kern_memset` |
| `heap_allocator_alloc` | `kern_heap_alloc` | `extend_heap` の分岐を落とす |
| `heap_allocator_free` | `kern_heap_free` | そのまま。`__heap_trace_pointer_walk`（`libc/heap.c:45-48` の weak）は `kern_heap_trace_pointer_walk` として `heap.h` で宣言し、`KERN_KERNEL_HEAP_TRACE` のときだけ `entry.c`（今の `:85-87`）が定義する（weak を使わない） |
| `heap_allocator_current/peak/largest_failed/largest_free` | `kern_heap_current/peak/largest_failed/largest_free` | そのまま |
| `heap_allocator_set_observer` | `kern_heap_set_observer` | `KERN_KERNEL_HEAP_TRACE` のときだけ（`entry.c:353-355` の利用がそれだけ） |
| `heap_allocator_validate` | `kern_heap_validate` | host 試験のために残す |
| `heap_allocator_trace_validate` | `kern_heap_trace_validate` | `KERN_KERNEL_HEAP_TRACE` のとき |
| `heap_allocator_reset`、`set_failure_after`、`set_grow`、`extend_heap`、`aligned_alloc`、`calloc`、`realloc`、`error_count` | 無し | kernel に利用者が無い（レビューでも再確認）。試験されない経路を kernel に持ち込まない。必要になれば libc 版から同じ形で足す |
| `heap_active_*`、`heap_strdup_active`、`malloc/calloc/realloc/free`、`__libc_heap_lock/unlock`（weak）、`errno` | 無し | libc 互換経路。kernel には malloc の呼出しが無い（grep で 0） |

`src/kern/heap.h`（private。`src/kern/net/internal.h` と同じく `src/kern/` 直下に置き、`entry.c` は `"heap.h"` で
include する）は `struct kern_heap`、`enum kern_heap_event`、`kern_heap_observer_fn`、上の関数を宣言する。
`include/kern/` には置かない（kernel 全体の API ではなく entry.c の実装詳細）。

`src/kern/entry.c` の変更点:
- `#include "libc/heap.h"` → `#include "heap.h"`、`<string.h>` → `<kern/kcrt.h>`、`memset` → `kern_memset`。
- `heap_allocator_*` → `kern_heap_*`、`heap_active_set(&kernel_heap)`（`:356`）を削除。
- `__libc_heap_lock/unlock`（`:98-150`）と `kernel_heap_libc_lock_active/irq_enabled`（`:57-58`）を削除
  （libc の malloc が kernel に無くなるので不要。lock domain は `kernel_heap_lock_enter/leave` に一本化される）。
- `kern_malloc/kern_calloc/kern_free/kern_memory_get_stats`（`include/kern/kmem.h`）の宣言・意味は変えない。
  `kernel_alloc/kernel_free`（HAL 向け、`:530-548`）も変えない。

## 9. 検査: kernel・HAL が読む libc ヘッダが 0 件であることの保証（設計項目 8）

二段構えにし、役割を分ける: `-M` 監査が compiler ヘッダの集合を、`.d` 検査が tree 内のヘッダを担う [R14]。

1. **監査（Phase の受入、`-M` 再実行）**: `plan/ws035/tests/kernel-include-audit.py` に `--require-none` を足す。
   `kernel` と `hal` の class について `libc` と `libc-internal` の集合が空、`other` が `bootloader/include/*` だけ、
   `compiler` が許可集合（`stdint.h stddef.h stdbool.h stdarg.h limits.h` と clang の `__stddef_*.h`/`__stdarg_*.h` の
   補助）に収まることを検査し、違えば非 0 で終了する。`classify()` は sysroot・`include/libc/`・p023 後の
   `include/<libc の最上位>` を全部 `libc` に分類するので、p023 の後も同じ検査が働く。**p023 が libc ヘッダを
   `include/` 直下に置いたまま kernel の `-Iinclude` を残せば、この検査は失敗する。それを緑にするのは p023 の
   設計の責任である（§14.6）** [R9]。
2. **常設検査（build に組み込む）**: `platform/amd64/tools/check-kernel-includes.noct`（既存の
   `check-amd64-vmunix.noct` と同じ流儀の Noct script）を `$(BUILD)/vmunix` の recipe（`vmunix.mk:308-314`）で
   link の直後に実行する。入力は **link 対象の object に対応する `.d`**（`$(AMD64_VMUNIX_OBJS:.o=.d)` のうち存在する
   もの。`.S` の rule は `-MMD` を付けないので `.d` が無い）に限り、config 切替で残った古い object の `.d` を
   拾わない。各依存パスが `include/kern/`、`include/hal/`、`include/drivers/`、`include/uapi/`、`include/boot/`
   （p004 までは残る）、`src/`、`bootloader/include/`、`plan/`（`pci-msi-qemu.c` の自ファイル）のいずれかで
   始まらなければ失敗し、違反したパスと object を列挙する。resource dir のヘッダは system 扱いで `.d` に現れない
   ので、その集合は 1 が担う [R14]。
   加えて make の段階で `$(if $(filter libc/% src/libc/%,$(AMD64_VMUNIX_OBJS:$(BUILD)/kern64/%.o=%.c)),$(error …))`
   により libc の source が link 一覧に入らないことを固定する。
3. **link の検査（p034 の受入で一度、以後は 2 に含める）**: `llvm-nm --defined-only $(BUILD)/vmunix` に、
   baseline の libc object が定義していた symbol（`heap_active_set`、`malloc`、`snprintf`、`strlen`、`setlocale`、
   `regcomp` …。baseline の `kern64/libc/*.o` から機械的に作る一覧 `plan/ws035/phase034/libc-symbols.txt`）が
   `memcpy`・`memset`（kcrt が定義）を除いて存在しないこと。
4. **ABI 値の台帳（p035）**: `uapi-abi-layout-check` には rule が無い（§1）ので、代わりに
   `plan/ws035/tests/uapi-value-ledger.py` を新設する。移動前の tree で、移す macro の値と、移す構造体の `sizeof`/
   `offsetof` を、target clang（`x86_64-unknown-zedbsd` と `i386-unknown-zedbsd`、`-DKERN_USER_ABI_LP64` の有無）で
   `_Static_assert` を生成して台帳化し、移動後に同じ台帳を compile して照合する。ioctl 番号（`_IOR` の展開値）も
   台帳に含める [R3]。
5. **否定試験（p035 の受入で一度）**: kernel の flags で `#include <stdio.h>` を含む scratch TU を compile して
   「file not found」で失敗すること（p023 前）と、`include/stdio.h` を含む偽の `.d` を 2 の script に与えて失敗する
   ことを確かめる。

抜け道の扱い: `#include "../../libc/..."` や `"include/libc/..."` の相対 include は `.d` に `libc/…` のパスで現れる
ので 2 が捕まえる。生成ヘッダ（`$(BUILD)` 配下）は今の kernel には無く、現れたら `$(BUILD)/` の接頭辞で 2 が失敗する。
`i915-old/` は build されないので対象外。HAL の object は 1・2 の両方に含まれる。

## 10. p034 と p035 への分け方、受け入れ条件と確認手順（設計項目 9）

方針: 一度に 1 つの変数だけを変える。p034 は「link から libc を外す」（ヘッダはまだ sysroot から読む）、
p035 は「ヘッダを外す」。p034 で出る link error は runtime の欠落だけ、p035 で出る compile error はヘッダの欠落
だけになり、原因が混ざらない。

### 10.1 p034: kcrt と heap、vmunix への libc の link をやめる

前提: p002 完了（`include/drivers/` の新パス）。ファイル範囲: `include/kern/kcrt.h`、`include/uapi/hosted.h`（switch だけ）、
`src/kern/{kcrt.c,heap.c,heap.h,entry.c,klog.c,io.c}`、`src/kern/platform/{x68k,sun4u}.c`（`hal_memset` 4 箇所）、
`src/kern/locale-record.c`（削除）、`platform/amd64/vmunix.mk`、置換対象の kernel source（§3.2）、
`plan/ws031/handover/tools/gen_vk_server_codec.py`、`plan/ws035/tests/`。

手順:
1. `include/kern/kcrt.h`、`include/uapi/hosted.h`、`src/kern/kcrt.c`（§3、§4。実装は `libc/string.c` と `src/kern/klog.c`
   の engine を出発点にした自前実装。§13）、`src/kern/heap.{c,h}`（§8.1）、`entry.c`・`klog.c` の変更。
2. `plan/ws035/tests/kcrt-rewrite.py` で呼出しを `kern_*` に置換し、`#include <kern/kcrt.h>` を挿入（§3.2）。
   生成器 `gen_vk_server_codec.py` を直して `vulkan-codec.inc` を再生成し、置換結果と一致させる。件数とファイル一覧を
   `plan/ws035/phase034/rewrite.json` に残す。i915-old と host harness は触らない。
3. `vmunix.mk`: `AMD64_KERNEL_LIBC_OBJS` を link から外す、kcrt.c/heap.c を足す、`locale-record.c` を外す、
   `AMD64_CFLAGS` に `-fno-builtin`。`AMD64_CPPFLAGS`（`-isystem sysroot`）はこの Phase では変えない。
4. link error に現れた標準名（予測 `memcpy`、`memset`）を kcrt.c に alias で定義し、kernel と HAL の全 object の
   暗黙参照を `plan/ws035/phase034/implicit-calls.txt` に記録する。
5. host 試験: `plan/ws035/tests/kcrt-test.c`（§3.3 の flags。各関数の境界: 長さ 0、重なり（memmove）、NUL 含み、
   `strncpy` の NUL 埋め、`strnlen` の上限、`memchr` の 0 値、`snprintf` の切り詰め・戻り値・`capacity 0`・`%zu`/`%#x`/
   `%016llx`/`%p`/`%%`/未対応変換での引数消費、`memset_explicit` が消えないこと（生成 asm に store が残る）。
   通常と ASan/UBSan）、`plan/ws035/tests/kern-heap-test.c`（`src/kern/heap.c` を include: 初期化の揃え、alloc/free と
   merge、`largest_free`、二重 free で `errors` 増加、`validate`、`-DKERN_KERNEL_HEAP_TRACE` で `trace_validate` が
   壊れた chain を追わずに拒否すること（`plan/ws002/tests/heap-trace-validator.c` の kernel 版））、
   `plan/ws035/tests/kcrt-format-scan.py`（§4）。
6. build と boot。

受け入れ条件（すべて記録し、観測しなかったものを PASS と書かない）:
- `make BUILD=build/ws035-p034 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix` と i915 構成
  （`plan/ws029/tests/config-i915-amd64.mk`）が warning 0 で通り、`amd64 vmunix check: PASS`（`timeout 1200`）。
- `$(BUILD)/vmunix` の link 一覧に `libc/` の object が無い。§9 の 3 の symbol 検査 PASS。`llvm-nm -u kcrt.o` が空。
- 置換 script の再実行で差分 0（決定性）。生成器で再生成した `vulkan-codec.inc` が tree と一致。`git diff --stat` の
  対象が §3.2 の範囲に収まる。
- host 試験 5・通常＋ASan/UBSan PASS（各 `timeout 120`）。`kcrt-format-scan.py` が許可集合外 0 件。
- host fixture の回帰: kernel source を compile する既存 fixture のうち、p002 が受入に使った一覧
  （`plan/ws035/phase002/host-tests/` の log にあるもの: GPU core、gpu-fence、PCI service、i915 contracts など）と、
  `src/kern/entry.c` を compile する `plan/ws018/tests/run-legacy-bootfs-removal-host-test.sh`（`heap.c`・`kcrt.c` の
  追加が要る）を同じ手順で実行し、p002 完了時の結果（PASS/FAIL）と一致すること（ws018 は修正して PASS）。
  p002 時点で失敗している fixture（例 `run-dp-host-test`、`run-lcd-host-test` の `drv_i915_perf_*` 未定義）は同じ
  失敗のままでよい [R15]。
- QEMU: `make -j16 disk-image`（既定の `build/amd64`、`config/ci/config-amd64.mk`）の後、
  `plan/tools/boot-test.sh`（2026-09-23に `qemu-base-utility-smoke.sh` を置き換え。UEFIのUSB起動とscreenshot）が
  PASS。これは init 起動と基本 utility の実行まで含む（`kern_malloc`・`kern_snprintf`・`kern_logf` の実動）。
  console log に `boot: starting init` があること。
- `git diff --check` PASS。規約（`plan/coding-style.md`）を新規ファイルに適用（forward declaration、purpose comment、
  `Succeeded:` の return、for 初期化子なし、file-scope 変数のコメント）。
- 未変更: `include/kern/kmem.h`、HAL、libc、UAPI（`hosted.h` の新設を除く）。

### 10.2 p035: kernel の include 整理と検査

前提: p034 完了。ファイル範囲: `include/uapi/*`（新規 12・拡張 3・連鎖修正 15）、`include/libc/*`（uapi を include
する側への変更、Vulkan の wrapper）、kernel/driver の include 行、HAL の include 行（§8 の表、承認済み範囲）、
`platform/amd64/vmunix.mk` の flags、`platform/amd64/tools/check-kernel-includes.noct`、`plan/ws035/tests/kernel-include-audit.py`、
`userland/base/libvulkan/tools/maintain-dispatch.noct` と文書のパス、`-Iinclude/libc` の fixture script（`-DKERN_UAPI_NATIVE` の追加）。

手順:
1. `plan/ws035/tests/uapi-move-ledger.py` で移す要素を確定し、`uapi-value-ledger.py` で移動前の台帳を作る（§9 の 4）。
2. uapi ヘッダの新設・拡張（§6.4、§8）。libc の対応ヘッダを「uapi を include して prototype と libc 固有定義を残す」
   形に変える。libc 側の値・layout は 1 つも変えない（移すだけ）。
3. uapi 内の連鎖（`sys/ioctl.h`・`sys/time.h`・`sys/types.h`）を uapi へ。
4. Vulkan の移動（§7）と tool・文書の更新、`dispatch-table.inc` の再生成一致。
5. kernel/driver/HAL の include 行の置換（§8）。`kcrt-rewrite.py` に include 行の規則を足して機械的に行う
   （`<string.h>`/`<stdio.h>` の削除を含む）。
6. flags（§6.2）と sysroot 依存の削除、`check-kernel-includes.noct` と audit の `--require-none`。
7. `-Iinclude/libc` の fixture に `-DKERN_UAPI_NATIVE` を追加（一覧を `plan/ws035/phase035/native-fixtures.txt`）。
8. build → 監査 → 台帳照合 → 否定試験 → userland → QEMU。

受け入れ条件:
- amd64・i915-amd64 の vmunix が warning 0 で通り、`check-kernel-includes.noct` が recipe 内で PASS（違反 0）。
  `clang -v` の search list に sysroot の `usr/include` が無い。
- `kernel-include-audit.py --require-none`: `kernel`・`hal` の `libc`/`libc-internal` が空、`other` は
  `bootloader/include/*` だけ、`compiler` は許可集合内。結果 JSON を `plan/ws035/phase035/include-audit/` に置く。
- `uapi-value-ledger.py` の照合が両 triple で PASS（macro の値、`sizeof`/`offsetof`、ioctl 番号が移動前後で同一）。
- 否定試験 2 件（§9 の 5）が期待どおり失敗する。
- `make BUILD=… vmunix` が sysroot 無しで通る（`sysroot-amd64` を作らない tree で kernel だけ build できる）。
- userland: `sh plan/ws035/tests/baseline-userland.sh amd64 …`（p001 と同じ条件）が warning 0 で通り、
  `rootfs-bin` の生成物一覧が p001 の `plan/ws035/phase001/baseline/` と一致（libc ヘッダの wrapper 化で userland が
  壊れていない証拠）。sysroot の `.zedbsd-sysroot-manifest` の `usr/include/` 部分は変わる（uapi の追加、Vulkan）。
  差分を `plan/ws035/phase035/sysroot-manifest.diff` に記録する。
- host fixture の回帰: p034 と同じ一覧が同じ結果。加えて `-Iinclude/libc` の fixture の代表（`plan/ws001/tests/directory-fsync-host-test.mk` の
  試験 1 本と、`-nostdinc` 付きの `plan/ws004/tests/run-intel-ax211-core-test.sh`）が PASS。`KERN_UAPI_HOST_LIBC` の
  分岐を `plan/ws035/tests/uapi-hosted-test.c`（host の `<errno.h>` と `<uapi/errno.h>` を同時に include して
  compile できる、`-DKERN_UAPI_NATIVE` では zedBSD の値になる、`-Iinclude/libc` かつ `-D` 無しでは `#error` になる）
  で示す [R1]。
- QEMU: p034 と同じ smoke が PASS。
- `git diff --check` PASS、規約適用。HAL の差分が §8 の表の行だけであること（`git diff src/hal include/hal` を記録）。
- 未変更: HAL の宣言・実装、`include/kern/kmem.h`、libc の実装（`libc/*.c`）。

### 10.3 見積と有限化

p034 は約 300〜420 active minutes（置換は script、link error の追跡、fixture 回帰の実行時間が主）、p035 は
約 360〜480 active minutes（uapi 13 ファイルの作成と台帳、300 ファイルの include 行置換、fixture 111 ファイルの
`-D` 追加、否定試験、userland build）。各 command は `timeout`（build 1200 s、host 試験 120 s、QEMU 300 s）、
同条件無変更の retry は 3 回まで。120 分ごとに進捗と残件を記録する。

## 11. 並行性・割込み・DMA・失敗と回復

- kcrt: 共有状態を持たない純関数。lock 不要、IRQ 文脈・NMI 相当・初期化中から呼べる。`kern_vsnprintf` の stack
  使用は小さい（数字 buffer 24 byte）。`kern_logf` の 512 byte buffer は現状どおり stack（`-Wframe-larger-than=8192`
  の範囲）。
- heap: lock を持たない（現状どおり）。`entry.c` の `kernel_heap_lock_enter/leave`（IRQ 無効 + spin）が唯一の
  lock domain になる。libc 互換の `__libc_heap_lock` 経路が消えるので、再帰 lock の検出（`HAL_FATAL("recursive libc kernel heap lock")`）
  も不要になる。
- DMA: kcrt/heap は DMA を扱わない。`memset_explicit` は cache 操作を含まない（現状の libc 版も同じ）。
- 失敗: kcrt は契約違反（NULL、範囲外）を検出しない（C と同じ）。`kern_snprintf` は `capacity 0` と `NULL format`
  を安全に扱い、未対応の変換でも引数を消費して以降をずらさない（§4）。heap は枯渇で NULL、foreign pointer の free で
  `errors++`（`kern_free` 側は `HAL_FATAL`。現状どおり）。
- 回復: 変更なし。kernel の他の部分（panic、watchdog）に影響しない。

## 12. 既存フレームワークとの関係

- **drv_gpu / i915 / venus**: 呼出しの `kern_*` 化と include 行の変更だけ。ioctl 番号（`_IOR` の値）は
  `uapi/ioctl.h` へ macro を移すだけで変わらない（p035 の台帳検査で固定）。i915 の log 文言は変わらない
  （§1: log macro は引数を書式化しない）。
- **cdev / devfs**: `snprintf("gpu%u")` 等が `kern_snprintf` になる。
- **PCI / USB**: 同上。
- **HAL**: §2。`hal_memcpy` 等は使わない。HAL の C ランタイムの注記（`hal.h:40-44`「HAL と kernel の初期化段階だけ」）
  と整合する。
- **host fixture**: §3.3・§6.4 により source 互換（`-Iinclude/libc` の fixture は `-D` 1 つ）。

## 13. ライセンスと転記

- 新規ファイル（`kcrt.h`、`kcrt.c`、`heap.c`、`heap.h`、uapi の新ヘッダ、Noct script、試験）は `SPDX-License-Identifier: Zlib`、
  規約 §13 の copyright header。
- `src/kern/heap.c` は `libc/heap.c`（Zlib、同一著作者）の複製。冒頭に「`libc/heap.c` から複製し kernel 用に整理した。
  以後は別々に保守する」と記す。
- `src/kern/kcrt.c` の string 関数は `libc/string.c`（Zlib、同一著作者）の単純な byte loop を出発点にし、書式 engine は
  `src/kern/klog.c`（Zlib）から移す。第三者コードの転記は無い。
- uapi へ移す定義は `include/libc/*.h`（Zlib）からの移動で、値は変えない。
- Vulkan: Khronos 由来宣言（Apache-2.0）と zedBSD の選択 tool（Zlib）。ファイルの著作権表示・`LICENSE-API`・
  `API-PROVENANCE.md` をそのまま一緒に移す。転記ではなく移動。
- RTL8822B の `.inc`（ライセンス分離）は置換対象に掛からない（`src/drivers/**/*.inc` で string 関数を呼ぶのは
  `vulkan-codec.inc` だけ）。

## 14. 人間の判断が要る点と、発見した依存

1. **HAL/kernel 共通の compile flags の変更**（`AMD64_CPPFLAGS` の `-nostdinc -isystem` → `-nostdlibinc`、
   `AMD64_CFLAGS` の `-fno-builtin`）は HAL の object にも効く。HAL の source は変えない（include 行の変更は承認済み）。
   HAL の `.text` は `-fno-builtin` で変わらず、`task.c` は `-nostdlibinc` で compile できることを確認した（§1）。
   flags は `platform/amd64/vmunix.mk` にあり HAL 配下ではないが、承認の範囲に含まれるかを確認したい。
2. **Vulkan 宣言を `include/uapi/vulkan/` へ**（Apache-2.0 のファイルを uapi に置く。§7）。
3. **uapi の host fixture への委任規則**（§6.4。uapi ヘッダが host fixture では host の libc ヘッダを include する）と、
   `-Iinclude/libc` の fixture 111 ファイルへの `-DKERN_UAPI_NATIVE` の追加（`plan/*/tests` の script の機械的編集）。
4. **explicit な `memcpy`/`memset` の呼出しも `kern_*` に改名する**（約 2,300 行）。標準名は compiler 契約のためだけに
   残す。ユーザー決定に沿った案だが diff が大きいので確認したい。
5. **`src/kern/locale-record.c` の削除**（libc の locale.c を link するためだけの存在）と、kernel 側の `hal_memset`
   4 箇所の `kern_memset` への置換。
6. **p023 への依存（新規発見、kernel 側が主因）**: p023 が `include/libc/<R>` を `include/<R>` に移すと、
   `include/stdint.h`・`stddef.h`・`limits.h`・`stdarg.h`・`stdbool.h`・`errno.h`・`string.h` が kernel の `-Iinclude` で
   compiler の resource dir より先に見つかり、kernel は再び libc のヘッダを読む（§9 の検査が失敗する）。host fixture の
   `-Iinclude` も同様に zedBSD の libc ヘッダを glibc より先に拾い、§6.4 の委任規則が破れる。したがって p023 は、
   kernel と fixture が `include/` 直下を `-I` にしない設計（refactor-map の案 2: kernel 用 include 見取り図を
   `$(BUILD)/kernel-include/` 等に生成して kernel と fixture がそれを `-I` する、または libc ヘッダを `include/` 直下
   以外（例 `include/libc/`）に置き sysroot で `usr/include/` に平坦化する）を要する。root が p023 の計画に反映する
   [R9]。
7. `Makefile:470-475` の `uapi-abi-layout-check`・`posix-header-check` 等は rule が無い。p035 は §9 の 4 の台帳検査で
   代替する。これらの target を作るかは別の判断（WS036 か Future Work）。
8. arm64（`aarch64-unknown-zedbsd` は `__ZEDBSD__` を定義しない）と GCC の platform は、kernel・libc・userland の
   全 compile に `-DKERN_UAPI_NATIVE` を与えるか、clang patch を広げる必要がある（WS036）。

## 15. 選ばなかった案（まとめ）

| 案 | 理由 |
| --- | --- |
| kcrt を `__builtin_memcpy` への inline wrapper にする | ユーザー決定（自前実装）。小さい定数長 copy の inline 展開は失うが、可変長 copy は今も libc の byte loop（`libc/string.c:15-23`）への呼出しなので退行しない。測定して必要なら後で検討 |
| explicit な `memcpy` 等の呼出しを残し `kern_*` は他だけ | 境界が曖昧になる（kernel が標準名を直接呼び続ける）。ユーザー決定に反する |
| `libc/format.c` を複製して `kern_snprintf` にする | 602 行・locale/wide 依存。kernel の書式は §4 の部分集合で足りる |
| heap.c の全 API を残す | 利用者の無い経路（realloc、aligned_alloc、grow、失敗注入）を kernel に持ち込まない。libc 版は残る |
| Vulkan を例外として許可リストに載せる／kernel 専用複製 | §7 |
| fixture 167 script に `-D` や kcrt.c を足す | §3.3、§6.4（`-Iinclude/libc` の 111 ファイルだけは `-D` が要る） |
| uapi の名前を `KERN_EINVAL` 等にする | 改名の規模と方針不一致 |
| `-nostdinc -isystem <resource dir>` | clang では `-nostdlibinc` が同じことをより短く表す。GCC 向けは WS036 |
| p034 で flags も同時に変える | link error と compile error の原因が混ざる。段階を分ける |
| kernel 用 `include/kern/heap.h` として公開 | entry.c の実装詳細なので private（`src/kern/heap.h`） |
| 未対応の書式変換で引数を消費しない（現状の klog と同じ） | `-Wformat` が合法とする書式で以降の引数がずれ、`%s` で不正参照になり得る |
