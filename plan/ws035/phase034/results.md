# ws035-p034 結果: kcrtとheap、vmunixへのlibcのlinkをやめる

Queue q318 / 項目 q318-i01。実行 2026-09-23 06:15〜07:00（+09:00）、`/home/awe/zedBSD-rpi4`。
開始時 HEAD `a567792d`（作業ツリーclean）。**git commit はしていない。** 実行中に root が計画ファイルだけを
commit した（`1ba5c756`、`74d31749`、`2023c02e`、`d5221d7a`）。`74d31749` には、このPhaseの途中の plan 側ファイル
（`plan/ws004/tests/pci-msi-qemu.c`、`gen_vk_server_codec.py`、途中のlog）が入っている。`src/`・`include/`・`platform/` の
変更は未commitで作業ツリーにある。

## 結果の要約

- `include/kern/kcrt.h`・`src/kern/kcrt.c`（kern_mem*/kern_str* 16個、`kern_snprintf`/`kern_vsnprintf`、標準名 `memcpy`/`memset` は
  alias）、`include/uapi/hosted.h`（switchだけ）、`src/kern/heap.{c,h}`（`libc/heap.c` の複製を kernel 用に整理）を作った。
  kcrt は HAL の C ランタイムを呼ばない。使い分けは `kcrt.h` 冒頭のコメントに書いた。
- `entry.c` は `kern_heap_*` に切り替え、`heap_active_set` と `__libc_heap_lock/unlock` を削除。`klog.c` の `kern_logf` は
  `kern_vsnprintf` で描画し、自前の書式engineを削除。`include/kern/kmem.h` は変更なし。
- 置換 script で kernel の呼出し 2,662 箇所を `kern_*` に、`hal_memset` 4箇所を `kern_memset` にし、229 ファイルに
  `#include <kern/kcrt.h>` を挿入（232 ファイル変更）。再実行で差分0。生成器を直し、`vulkan-codec.inc` は再生成と一致。
- `vmunix.mk`: libc の object を link から外し、`kcrt.c`・`heap.c` を足し、`locale-record.c` を外し（ファイルも削除）、
  `AMD64_CFLAGS` に `-fno-builtin`。`AMD64_CPPFLAGS`（`-isystem sysroot`）は変えていない。
- amd64・i915-amd64 の vmunix が warning 0 で通り、link 一覧に libc の object は0、libc の symbol も0（`memcpy`/`memset` を除く）、
  `llvm-nm -u kcrt.o` は空。libc を外した link が要求した標準名は `memcpy` と `memset` だけ（実測）。
- host 試験（kcrt・heap、通常と ASan/UBSan、heap は trace build も）PASS。書式scanは engine に届く 1,213 呼出しで許可集合外0。
- host fixture 46 本の回帰: p002 時点（HEAD の export）と終了状態・正規化した error 集合が 46/46 一致。
- QEMU: 指定の `qemu-base-utility-smoke.sh` は **p034 と p002 の両方の kernel で同じく timeout で失敗**（既存の不整合。下記）。
  同じ操作を COM1 から読む写し（`plan/ws035/tests/qemu-base-utility-smoke-serial.sh`）は p034・p002 とも PASS。

## 実行したコマンドと結果

### 1. 実装（手順1）

新規: `include/kern/kcrt.h`、`include/uapi/hosted.h`、`src/kern/kcrt.c`、`src/kern/heap.c`、`src/kern/heap.h`。
変更: `src/kern/entry.c`、`src/kern/klog.c`。削除: `src/kern/locale-record.c`。

- kcrt の書式: 変換 `d i u x X c s p %`、flag `0`・`#`、10進の幅、長さ `hh h l ll z`。それ以外（精度、`*` 幅、`- + 空白`、
  `c s p` への長さ、`o n j t L`、浮動小数点）は `%` と変換文字を出し、引数は C の規則で消費する（浮動小数点は double を
  取り出さない）。幅は 4096 で打ち切る。`format == NULL` は `""`、`capacity 0` は何も書かず長さを返す、長さが int に
  入らなければ -1。
- `kcrt.h` は zedBSD target（`__ZEDBSD__`）では宣言、host fixture（`KERN_UAPI_HOST_LIBC`）では host の libc への
  `static __inline` wrapper。`strnlen` は POSIX で `-std=c11` の fixture から見えないため、host 面では自前の loop にした。
- `heap.c`: `libc/heap.c` の first-fit・split・merge・統計・`validate`・`trace_validate` を残し、grow・失敗注入・realloc・
  aligned alloc・active heap・malloc 互換を落とした。trace の weak hook は `kern_heap_trace_pointer_walk`（`heap.h` で宣言、
  trace build の `entry.c` が定義）に置き換えた。observer の enum は `KERN_HEAP_EVENT_*`（libc の `KERN_HEAP_ALLOCATED` と衝突させない）。

### 2. 置換（手順2）

- `python3 plan/ws035/tests/kcrt-rewrite.py`（決定的。C を字句解析し、comment・文字列・文字定数の中は置換しない）。
- `rewrite.json` は、HEAD を `git archive` した木に置換前の手作業分（`entry.c`・`klog.c`・新規ファイル・`locale-record.c` 削除）
  を載せて script を走らせたものを採った。その木の結果は作業ツリーの scope 658 ファイルとすべて一致し、2回目は `would_change=0`。
- `rewrite.json` の要約: scope 658、変更 232、置換 2,662（memset 1,548、memcpy 720、memcmp 107、strlen 75、strcmp 73、
  strcpy 57、strncmp 22、strchr 15、strncpy 10、snprintf 7、memchr 5、strcat 5、strrchr 5、memmove 4、hal_memset 4、strnlen 3、
  strstr 1、memset_explicit 1）、include 挿入 229、include 削除 0。comment 内で置換しなかったもの 2（`dp-internal.h:787,804`）。
  include 挿入先の無い断片 `render/vulkan-codec.inc` は、include する 7 ファイル（`render/{command,descriptor,image,instance,memory,pipeline,render-pass}.c`）に挿入。
  `display/capture.c` は全体が `#ifdef I915_TEST_CAPTURE` の中なので、その中の最初の include block の後に挿入。
  `include/kern/**`・`include/drivers/**` には置換対象の呼出しが無かった。
- `python3 plan/ws035/tests/kcrt-rewrite.py --check` → `files=658 would_change=0`（最終状態でも再確認）。
- 生成器 `plan/ws031/handover/tools/gen_vk_server_codec.py` の emit 5行を `kern_memcpy(` に変更。変更前に、変更前の生成器が
  tree の `vulkan-codec.inc` を再現することを確認し、変更後の再生成（`userland/base/libvulkan/codec.c` だけを置いた一時木）が
  置換後の tree と `cmp` で一致（`kern_memcpy(` 21箇所）。

### 3. build（手順3、各 `timeout 1200`、`JOBS=32`）

`sh plan/ws035/tests/refactor-build.sh p034 <name> <config> vmunix`（sysroot と Noct は `build/ws035-p002/` から `cp -a` した私用の写し）。

| name | config | 結果 |
| --- | --- | --- |
| amd64 | `config/ci/config-amd64.mk` | status 0、warning 0、`amd64 vmunix check: PASS`、185 object |
| i915-amd64 | `plan/ws029/tests/config-i915-amd64.mk` | status 0、warning 0、`vmunix check: PASS`、247 object |

最終 source で `build/ws035-p034/{amd64,i915-amd64}` を消してから build した log が `build/amd64.log`・`build/i915-amd64.log`。
最初の build の log（`build/amd64-first-with-sysroot-refresh.log`）には warning が1件あるが、`include/uapi/hosted.h` の追加で
私用 sysroot が更新されたときの `-no-pie`（sysroot 側の既存の警告、p001/p002 と同じ）で kernel ではない。
object 数は p002 の 215／277 から libc 31 個を除き kcrt・heap を足し locale-record を除いた数と一致する。

### 4. link の検査と暗黙の呼出し（手順4、§9 の 3）

`python3 plan/ws035/tests/kcrt-link-check.py --baseline build/ws035-p002/amd64 --baseline build/ws035-p002/i915-amd64 --symbols plan/ws035/phase034/libc-symbols.txt --implicit plan/ws035/phase034/implicit-calls.txt amd64=…=build/ws035-p034/amd64 i915-amd64=…=build/ws035-p034/i915-amd64`（log `link-check.log`）:

- link 一覧の libc object: 両構成 0。
- p002 の `kern64/libc/**/*.o` が定義していた global symbol 480個（`libc-symbols.txt`）のうち vmunix にあるもの（`memcpy`・`memset` を除く）: 両構成 0。
- `llvm-nm -u kcrt.o`: 両構成とも空。`kcrt.o` は `memcpy`・`memset`・`kern_memcpy`・`kern_memset` を定義。
- 標準名を外した probe（`kcrt.c` を `-DKERN_KCRT_NO_STANDARD_NAMES` で compile し直し、同じ link を再実行）: link は失敗し、
  未定義は `memcpy` と `memset` だけ（両構成）。設計の予測どおり。`memmove`・`memcmp` は現れなかったので定義していない。
- `implicit-calls.txt`: 標準名を参照する object は amd64 19（kernel 17、HAL `task.o`・`bsp-pcat/boot.o`）、i915-amd64 14（kernel 12、HAL 同じ2個）。
  すべて source に文字どおりの呼出しの無い compiler 生成の参照（`implicit`）。

### 5. host 試験（手順5、各 `timeout 120`）

- `sh plan/ws035/tests/run-kcrt-host-tests.sh`（clang、log `host-tests-kcrt.log`）: PASS。
  - kcrt: `kcrt.c` を `-ffreestanding -fno-builtin -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES` で別 compile。`nm -u` に
    memcpy/memset/memmove/memcmp 無し。`kcrt-test.c` 105 checks、通常と ASan/UBSan で PASS（長さ0、memmove の両方向の重なり、
    NUL を含む memchr、strncpy の NUL 埋めと非終端、strnlen の上限、strchr/strrchr の NUL、strstr の空 needle、支持する書式を
    host の snprintf と byte 単位で比較（`%zu %zx %zd %#x %#X %#08x %016llx %05d(-42) %hhu %hd %5s` 等、切り詰めと戻り値）、
    `%p`、`(null)`、capacity 0、NULL format、未対応書式の引数消費（`%-5d` `%.*s` `%*d` `%o` `%llo` `%+d` `% d` `%n` `%ls` `%lc` `%y` `%f` 末尾の `%`）、幅の上限）。
  - `kern_memset_explicit`: 最後の使用後に消去する probe を `-O2` で asm にし、消去の store が 4 命令残る（対照の `kern_memset` は 0 で消える）。
  - heap: `kern-heap-test.c`（`src/kern/heap.c` を include）通常 55・ASan/UBSan 55、`-DKERN_KERNEL_HEAP_TRACE` 77・その ASan/UBSan 77 checks PASS
    （初期化の揃え、alloc/free と merge、largest_free、largest_failed、二重・範囲外・block 内部の free で errors 増加、validate の否定、
    trace_validate が loop・範囲外・overflow・位置ずれ・偽の free block・逆リンクの誤りを追わずに拒否、observer と walk hook、ループした鎖での free が止まる）。
- `CC=cc`（gcc 14）でも同じ script が PASS（`host-tests-kcrt-gcc.log`）。
- 書式scan `python3 plan/ws035/tests/kcrt-format-scan.py`（`format-scan.json`、`format-scan.log`）: engine の sink は `kern_logf`・
  `kern_snprintf`・`kern_vsnprintf` と、それに format を渡す macro（`VFS_LOG`、`DRM_INFO`、`I915_HPD_DRM_*` 等 8個、不動点で検出）。
  文字列 macro（`DRM_MODE_FMT` 等）は展開して走査。engine に届く 1,213 呼出し、許可集合外 **0**。使われている変換は 28 種
  （`%u %d %s %08x %02x %x %llu %llx %04x %c %016llx %p …`）。literal でない format は `klog.c` の転送 1 件だけ。
  compile 時の型検査だけの `drv_i915_vbt_fmtcheck`/`drv_i915_lcd_fmtcheck` 系（I915_VBT_LOG・I915_DP_LOG 等、engine に届かない）は
  参考として別に数え、240 呼出しのうち許可集合外 7（`%*p` 5、`%.*s` 2）。

### 6. host fixture の回帰

`sh plan/ws035/tests/run-p034-fixture-regression.sh /tmp/p034/baseline-head plan/ws035/phase034/host-tests`（各 `timeout 300`）。
baseline は HEAD（= p002 完了時）を `git archive` した木、current はこの作業ツリー。対象は p002 が使った一覧（contracts、vk host、
GPU core 11、PCI service、build selection 2、mview、ws031 表示系 6、ws029 i915 host、`related` の 21）と ws018 の
`run-legacy-bootfs-removal-host-test.sh`、計 46 本。

- 終了状態: 46/46 一致（PASS 21、FAIL 25、両方の木で同じ）。
- `error-set-compare.tsv`: 各 log の error 行の集合（行番号・object offset・一時パスを正規化）も 46/46 一致。
  最初の error 行の違いは、include 挿入による行番号 +1 と object offset だけ。
- 1 本だけ修正した: `plan/ws004/tests/run-intel-ax211-pci-test.sh` は `intel-ax211.c` の一部を切り出した断片
  （`plan/ws025/temp/…`、`prepare-driver-fragments.py` が生成、ファイルの include block を含まない）を compile するため、
  置換後は `kern_memset` の暗黙宣言で compile 段階で止まった。fixture `plan/ws004/tests/intel-ax211-pci-test.c` に
  `#include <kern/kcrt.h>` を1行足し、baseline と同じ既存の link error（`kern_logf` 未定義）まで進むことを確認した。
- ws018 `run-legacy-bootfs-removal-host-test.sh` は両方の木で失敗: `src/kern/system-device.c`（`src/drivers/generic/` へ移動済み）を
  grep するため。path を直した写しで試すと、次は `ZEDBSD_HANDOFF_MAGIC`（`KERN_HANDOFF_MAGIC` に改名済み）で止まる。
  p034 と無関係の古い試験で、この script は `src/kern/main.c` を compile し、`entry.c` は compile しない（設計の「heap.c・kcrt.c の
  追加が要る」は前提が違った）。直していない（残課題）。`entry.c`・`klog.c`・kernel heap を compile する fixture は他に無い。

### 7. QEMU（手順6）

disk-image（最終 source、`BUILD=build/ws035-p034/amd64`、`config/ci/config-amd64.mk`、`timeout 1800`）: status 0、warning 0
（`build/amd64-disk-image.log`。最初の disk-image では userland 側の警告 3 件（noct interpreter の `-Wreturn-type`、`-no-pie`、
gmake jobserver）が出た。kernel ではない）。

- `plan/ws001/tests/qemu-base-utility-smoke.sh`（`BOOT_TIMEOUT_SECONDS=120`、外側 `timeout 300`、`build/amd64` を一時的に
  `ws035-p034/amd64` への symlink にして実行し、後で削除）: **失敗（timeout）**。debugcon の log は `boot: starting init /sbin/init`
  で止まり、`login:` が来ない。
- 原因の切り分け: 同じ image を `-serial file:` 付きで起動すると COM1 に `init: started syslogd/networkd/cron/getty_console`、
  `init: system running`、`login:` が出る（`qemu/probe-com1-p034-serial.log`）。PC/AT の text console の mirror は
  `ca248d02`（2026-09-21）から COM1（`src/drivers/platform/pcat/serial-mirror.c`）だけに出ており、この smoke（2026-09-11）は
  `-serial none` で debugcon だけを読む。**p002 の kernel（`build/ws035-p002/amd64/vmunix`、libc を link した HEAD 版）を同じ
  userland に組んだ image でも、同じ smoke が同じく timeout で失敗した**（`qemu/qemu-smoke-base-001/`）。p034 の回帰ではない。
- 同じ session を COM1 から読む写し `plan/ws035/tests/qemu-base-utility-smoke-serial.sh`（違いは `-serial file:` と `IMAGE=`、
  fatal 検査に debugcon も含める、だけ）:
  - p034 image: **PASS**（`qemu/qemu-serial-p034-001/`）。debugcon に `boot: starting init` 1回、COM1 に `login: root`、
    `p017guest`・`p018guest`・`p016guest` 各1回、fatal/panic 無し、image の hash 不変。
  - p002 kernel の image: PASS（`qemu/qemu-serial-base-001/`）。
  - 両者の debugcon と COM1 の log の差は、image の UUID、kernel image の大きさ（`0x701000` → `0x6ff000`、8 KiB 小さい）、
    timer 較正値とそれに伴う空きメモリの数値だけ。他の kernel log 行（`kern_snprintf` の `event%u` 等を含む）は同一。
- QEMU は TCG（KVM なし）。実機では試していない。
- 調べ方: root の「QEMUの不具合解析」方針（gdbstub・monitor・`-d int,cpu_reset,guest_errors`）はこの切り分けの後に届いた。
  実際に使ったのは log の取得で、boot は切り分けのために 2 回だけ追加した（同じ p034 image を `-serial file:` 付きで 150 秒、
  p002 kernel の image で元の smoke を 1 回）。前者で guest が固まっておらず COM1 に出力していることが直接わかったため、
  gdbstub での停止・register の確認は行っていない（固まり・例外は起きていなかった）。

### 8. `git diff --check`

追跡ファイルは `git diff --check` PASS。新規ファイル 12 個は `git diff --no-index --check /dev/null <file>` で空白の問題なし。

## 変更の概要

| 種類 | ファイル |
| --- | --- |
| 新規（kernel） | `include/kern/kcrt.h`、`src/kern/kcrt.c`、`src/kern/heap.c`、`src/kern/heap.h` |
| 新規（UAPI、許可済み） | `include/uapi/hosted.h` |
| 変更（kernel） | `src/kern/entry.c`、`src/kern/klog.c`、置換 232 ファイル（`rewrite.json` の `files`。`src/kern`、`src/drivers`、`plan/ws004/tests/pci-msi-qemu.c`） |
| 削除 | `src/kern/locale-record.c` |
| build | `platform/amd64/vmunix.mk`（libc object を外す、kcrt/heap、locale-record を外す、`-fno-builtin`） |
| 生成器 | `plan/ws031/handover/tools/gen_vk_server_codec.py`（emit 5行） |
| fixture | `plan/ws004/tests/intel-ax211-pci-test.c`（`#include <kern/kcrt.h>` 1行） |
| 道具・試験 | `plan/ws035/tests/{kcrt-rewrite.py,kcrt-link-check.py,kcrt-format-scan.py,kcrt-test.c,kern-heap-test.c,run-kcrt-host-tests.sh,run-p034-fixture-regression.sh,qemu-base-utility-smoke-serial.sh}` |
| 記録 | `plan/ws035/phase034/{rewrite.json,implicit-calls.txt,libc-symbols.txt,link-check.log,format-scan.json,format-scan.log,host-tests-kcrt*.log,build/,host-tests/,qemu/,results.md}` |

HAL（`src/hal`、`include/hal`）、`include/kern/kmem.h`、libc、既存の UAPI は変更なし（`git diff a567792d` で確認。
libc の差分 `libc/include/linux/*.h` は root の commit `1ba5c756` のもの）。

## 受け入れ条件の達成状況

| 条件（§10.1） | 状況 |
| --- | --- |
| amd64・i915-amd64 の vmunix が warning 0、`vmunix check: PASS` | 達成（最終 source の clean build） |
| link 一覧に libc の object が無い、§9-3 の symbol 検査、`llvm-nm -u kcrt.o` が空 | 達成（両構成） |
| 置換の再実行で差分0、`vulkan-codec.inc` が再生成と一致、diff が §3.2 の範囲 | 達成。§3.2 外の変更は上表の build・生成器・新規ファイル・道具と、fixture 1行（`intel-ax211-pci-test.c`） |
| host 試験 通常＋ASan/UBSan PASS、書式scanの許可集合外0 | 達成（engine 0。check-only の 7 は engine に届かない型検査用で別記） |
| host fixture の回帰が p002 時点と一致、ws018 は修正して PASS | 前半は達成（46/46、終了状態と error 集合）。**ws018 は未達**: p034 と無関係の古い path・改名で両方の木で失敗し、直していない |
| disk-image の後 `qemu-base-utility-smoke.sh` が PASS、`boot: starting init` | **指定の script は未達**（p002 kernel でも同じく失敗する既存の不整合）。COM1 版の写しで p034・p002 とも PASS、`boot: starting init` あり |
| `git diff --check` PASS、規約の適用 | 達成（新規 C ファイルに forward declaration、purpose comment、`Succeeded:` の return、for 初期化子なし、file-scope 変数・型のコメント、条件内の関数呼出し無しを自己確認。試験・道具の C は試験用として簡略） |
| 未変更: `kmem.h`、HAL、libc、UAPI（`hosted.h` を除く） | 達成 |

## 未実施の確認

- amd64 以外の build（範囲外。壊れてよい）。`platform/{pcat,pc98,x68k,sparcv9,arm64}/vmunix.mk` は削除した
  `locale-record.c` をまだ参照しており、kcrt・heap も入っていない。
- 実機での起動。QEMU は TCG の 1 回ずつ。
- `-DKERN_KERNEL_HEAP_TRACE` の kernel build と `KERN_CONSOLE_OUTPUT_TEST` の HAL build（host 試験で trace 版の heap は確認したが、
  その config の kernel は build していない）。
- i915 構成の disk-image と QEMU（受け入れ条件は amd64 の image のみ）。

## 残課題・人の判断が要る点

1. **QEMU smoke の不整合（既存）**: `plan/ws001/tests/qemu-base-utility-smoke.sh` は `-serial none` で debugcon だけを読むが、
   text console の mirror は `ca248d02` から COM1 に出るため、どの kernel でも login を待って timeout する。受け入れ条件に
   指定されている script なので、元の script を直すか（`-serial file:` で読む）、COM1 版を正とするかの判断が要る。
   p035 の受け入れも同じ smoke を使う。
2. **ws018 `run-legacy-bootfs-removal-host-test.sh`**: 移動済みの `src/kern/system-device.c` と改名済みの `ZEDBSD_HANDOFF_*` を
   参照していて失敗する（p034 前から）。直すか退役させるかは担当 WS の判断。設計 §10.1 の「entry.c を compile する」は誤り。
3. **root の設計追記との関係**: 実行中に `kcrt-design.md` の冒頭へ「`render/vulkan-codec.inc` と生成器は変えない」が加わった
   （Vulkan の扱いの文脈）。この Phase の依頼どおり生成器の emit 5行だけ `memcpy(` → `kern_memcpy(` に変え、構造体を扱う設計は
   変えていない。「一切変えない」の意味であれば、kernel が標準名 `memcpy` を直接呼ぶ例外が残ることになるので確認したい。
4. **i386 などへの依存（発見、WS036）**: `kcrt.c` を `i386-unknown-zedbsd` で compile すると `__udivdi3` を要求する
   （書式の 64 bit 除算）。i386 の kernel は今まで libc の `int64.c` からこれを得ていた。libc を外すと i386 には kcrt 側
   （または compiler-rt 相当）の供給が要る。
5. **共有 `build/` への書込み**: `Makefile` の `DATA_IMAGE := build/data.img` 等と、`make` を既定パスで呼ぶ一部の fixture
   （build selection など）が `build/{data.img,swapfile,zedimage-host,arch-images,sources,amd64/sysroot,i386,NoctLang,host-noct-state}`
   を作った。開始時には無かったので、終了時に削除した。
6. 書式の意味の変化（C に合わせた）: 幅つきの負の数（例 `%3d` の -5）は、旧 klog の `-  5` から C と同じ ` -5` になる。
   engine に届く書式での該当は `display/diagnostics.c:1402` の `%3d` 1 箇所だけ。未対応の書式は引数を消費するようになった（利用 0）。
7. `plan/ws035/phase034/build/*.log` と host-tests の一部は、root の commit `74d31749` に途中の版が入った後、最終版で上書きしている。
