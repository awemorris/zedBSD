# ws035-p033 敵対的レビューの結果と対応

対象: `plan/ws035/kcrt-design.md`（初版、2026-09-23）。レビュー担当: `design-reviewer`（Fable 5.1、High）。
レビューはソースを変更せず、調査用の probe は `build/ws035-p033-review/` だけを使った（レビュー報告による）。
設計担当（`driver-designer`）は各指摘の事実を自分でも確かめてから設計を改訂した（確認の command と結果は
「確認」欄）。改訂後の設計文書では、対応した箇所を `[R#]` で示している。

## 指摘と対応

| # | 重要度 | 指摘（要約） | 確認 | 対応 |
| --- | --- | --- | --- | --- |
| 1 | 高 | §6.4 の前提「`libc/include` は fixture の `-I` に無い」が誤り。`-I…/libc/include` を付ける fixture があり（レビューは 53 script）、委任先が zedBSD の `libc/include/errno.h` になって `<uapi/errno.h>` との循環で `E*` が未定義になる。`-nostdinc` 付きは委任先が無く即 error。p034/p035 の fixture 回帰一覧（p002 の GPU/i915/PCI）に含まれず受入をすり抜ける | `grep -rlE -- '-I[^ ]*libc/include' plan/*/tests src/drivers/gpu/i915/tests` → 111 ファイル（script・Makefile・source を含む数え方）、うち `nostdinc` を含むもの 33。例 `plan/ws001/tests/directory-fsync-host-test.mk:9`、`plan/ws004/tests/run-intel-ax211-core-test.sh:15` | **設計を直した。** §0・§1・§6.4: これらの fixture は zedBSD の libc ヘッダで compile しているので `-DKERN_UAPI_NATIVE` を付ける（p035 の作業、一覧を `native-fixtures.txt`）。委任先が zedBSD libc だった場合を `LIBC_ERRNO_H` 等の guard で検出して `#error` にする。§10.2 の受入に `-Ilibc/include` の代表 2 本と `uapi-hosted-test.c` の `#error` 確認を追加。「fixture の script を一切触らない」は撤回し §15 も修正 |
| 2 | 高 | §8「`sys/mount.h` は不使用」が誤り。`syscall.c:3846` が `MNT_RDONLY \| MNT_NOSUID`、`:3829,3858` が `struct mount_args`・`KERN_MOUNT_ARGS_VERSION` を使い、定義は `libc/include/sys/mount.h` にしか無い | `grep -n "MNT_RDONLY\|mount_args\|KERN_MOUNT_ARGS_VERSION" src/kern/syscall.c` → 3829、3830、3846、3858。`include/uapi` に `MNT_RDONLY`/`mount_args` は無い。初版の見落としは grep 出力を `head -5` で切ったため | **設計を直した。** §8 に新 `include/uapi/mount.h`（`MNT_RDONLY`/`MNT_NOSUID`、`KERN_MOUNT_ARGS_VERSION`、`KERN_MOUNT_FSPEC_MAX`、`struct mount_args`）を追加し、新規 uapi を 12 → 13 に訂正。`sys/socket.h`・`sys/select.h`・`sys/sysctl.h` は削除のまま |
| 3 | 高 | §10.2/§12 が依拠する `uapi-abi-layout-check` に rule が無い。`posix-header-check` 等も同様に未確認 | `timeout 60 make -n ZEDBSD_CONFIG=config/ci/config-amd64.mk uapi-abi-layout-check` → `No rule to make target`。`posix-header-check` も同じ | **設計を直した。** §9 に 4「ABI 値の台帳」（`plan/ws035/tests/uapi-value-ledger.py`: 両 triple・`KERN_USER_ABI_LP64` 有無で macro 値・`sizeof`/`offsetof`・ioctl 番号の `_Static_assert` を生成して移動前後で照合）を追加し、§10.2・§12 の `uapi-abi-layout-check`・`posix-header-check` への依存を消した。§1・§14.7 に「rule が無い」事実を記録 |
| 4 | 中 | 「i915 の `%zu`/`%#x` が現状は誤って出力され、kcrt で正しくなる」は誤り。`I915_VBT_LOG`/`I915_DP_LOG` は `if (0)` の fmtcheck で書式を検査するだけで引数を書式化せず、文字列を `drv_i915_vbt_note(level, fmt)` に渡す | `sed -n 203,212p src/drivers/gpu/i915/display/vbt.h`、`vbt.c:776` の `kern_logf("i915: vbt: %s", text)` を確認 | **設計を直した。** §1 の事実を書き換え、§4 の `z`/`#` の根拠を「`-Wformat` 適合の書式を将来渡せるようにする予防（費用小）」に改め、§12・§14.7 の「描画が正しくなる」を削除 |
| 5 | 中 | p034 の順序矛盾: `kcrt.h` が p035 で作る `uapi/hosted.h` に依存し、置換後の約 300 ファイルに `<kern/kcrt.h>` を include させる手順が無い（暗黙宣言で `-Werror`） | 設計文書の記述を確認（§3.3・§10.1 の範囲に `hosted.h` が無く、include 挿入の規則が無かった） | **設計を直した。** §3.2 に「置換したファイルへ `#include <kern/kcrt.h>` を挿入（最初の include block の末尾）、p034 は `<string.h>`/`<stdio.h>` を残し p035 で同じ script が削除」を追加、§10.1 の範囲と手順 1 に `include/uapi/hosted.h`（switch だけ）を追加、`rewrite.json` に挿入件数を記録 |
| 6 | 中 | §8 の移動一覧の漏れ: `GETENTROPY_MAX`（`syscall.c:5180,5188`）、`st_atime/st_mtime/st_ctime` macro、`PRIO_*`・`RUSAGE_*`（`syscall.c:8050-8112`）、`rlim_t`/`struct rlimit`/`struct rusage`、`MS_ASYNC`/`MS_INVALIDATE` | `grep -n GETENTROPY_MAX src/kern/syscall.c` → 5180、5188。`st_atime\|st_mtime` 8 行。`PRIO_PROCESS\|RUSAGE_SELF` → 8050、8058、8112。`MS_ASYNC\|MS_INVALIDATE` 16 行 | **設計を直した。** §8 の limits・stat・resource・mman の行に追加し、§6.1 に `GETENTROPY_MAX` を追記。着手時に `uapi-move-ledger.py`（libc ヘッダの定義名を抽出して kernel を grep し uapi に無いものを列挙）で機械的に補完することを §8 冒頭と §10.2 手順 1 に追加 |
| 7 | 中 | 「`__ZEDBSD__` は zedBSD target すべてで定義される」は aarch64 では成り立たない（`platform/arm64/vmunix.mk:6` の `--target=aarch64-unknown-zedbsd` で未定義）。arm64 は kernel だけでなく libc/userland も委任経路に落ちる | `echo \| clang --target=aarch64-unknown-zedbsd -dM -E - \| grep -c __ZEDBSD__` → 0 | **設計を直した。** §1・§6.4・§14.8: `__ZEDBSD__` は x86 の patched clang だけ。arm64 と GCC platform は kernel・libc・userland の全 compile に `-DKERN_UAPI_NATIVE` を与えるか clang patch を広げる（WS036）。kernel flags だけでは足りない点を明記 |
| 8 | 中 | 置換対象に生成物 `render/vulkan-codec.inc`（`memcpy(` 21）が含まれ、生成器 `gen_vk_server_codec.py:46-50` が `memcpy(` を emit するので再生成で戻る | `grep -c "memcpy(" src/drivers/gpu/i915/render/vulkan-codec.inc` → 21。生成器 `:46,48,50` を確認。`src/drivers/**/*.inc` で該当はこの 1 ファイル（RTL8822B は無関係） | **設計を直した。** §1・§3.2・§10.1: 生成器を `kern_memcpy(` を emit するよう直し、再生成との一致を p034 の受入に追加。対象拡張子 `.c/.h/.inc` を明記。§13 に RTL8822B `.inc` が対象外である旨を追記 |
| 9 | 中 | p023 との衝突は fixture だけでなく kernel の build にも起こる（`-Iinclude` が resource dir より先なので `include/stdint.h` 等が compiler ヘッダを隠す）。§9 の「p023 後も同じ検査が使える」は「p023 後に失敗する検査」 | clang の search order（`-I` → system/resource dir）は §6.2 の `-v` 出力で確認済み。refactor-map の p023 規則で `include/stdint.h` 等が生成される | **設計を直した。** §14.6 を kernel 側の理由を先に書く形に書き換え（案 2 か `include/libc/` 等の別置きが要る）、§9 の 1 の表現を「p023 が libc ヘッダを `include/` 直下に置いたまま `-Iinclude` を残せば失敗する。緑にするのは p023 の責任」に改めた。§6.4 にも kernel 側が壊れる旨を追記 |
| 10 | 中 | `-Wformat` は部分集合を強制できず、未対応変換（`%.3s`、`%-5d`、`%o`、`%*d`）で engine が `va_arg` を消費しないと以降の引数がずれる（現状の klog も同じ） | `klog.c:223-226` は `%` と文字を出すだけで `va_arg` を呼ばない（初版 §1 で引用済み） | **設計を直した。** §3.1・§4・§11: 未対応変換でも C の規則で引数を 1 つ消費する（長さ修飾に応じた型。`f/e/g` は `double` なので消費せず、呼出し元を scan で 0 件に保つ）。受入に `kcrt-format-scan.py`（`kern_logf`/`kern_snprintf`/fmtcheck の文字列リテラルを走査し許可集合外 0 件）を追加。§15 に「消費しない案」を却下理由付きで追加 |
| 11 | 低〜中 | kcrt.c の host 試験は `-ffreestanding`/`-fno-builtin` 無しでは host の cc が byte loop を `memcpy` 呼出しに変換し、glibc を試験することになる | §5 の probe（flags 無しで loop が memmove 化）と同じ機構 | **設計を直した。** §3.3・§10.1: `-ffreestanding -fno-builtin -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES` で compile し、`nm -u kcrt.o` に mem*/str* が無いことを試験に含める。ASan intercept が効かない点を明記 |
| 12 | 低 | HAL の libc 依存の実態が違う: `cons.c:243` の `memset` は `KERN_CONSOLE_OUTPUT_TEST` 内、通常 build では compile されない。実際は compiler 生成の `memcpy` が `task.o`・`boot.o` にある | `cons.c:215` の `#ifdef KERN_CONSOLE_OUTPUT_TEST` を確認、config に定義無し。`llvm-nm -u build/ws035-p001/amd64/src/hal/**/*.o` → `task.o`・`boot.o` に `memcpy`。加えて `task.c`・`lib.c`・`bsp-pcat/boot.c` を `-fno-builtin` 有無で compile し `.text*` の SHA-256 一致、`task.c` が `-nostdlibinc` で compile できることを確認 | **設計を直した。** §1・§2・§5・§8: HAL の `task.o`/`boot.o` が kcrt の `memcpy` を要求する（HAL→kcrt の唯一の依存）と明文化、`cons.c` の include 変更は test build のため、`implicit-calls.txt` に HAL object を含める。§14.1 に `.text` 不変の確認結果を追記 |
| 13 | 低 | kernel 側の既存 `hal_memset` 呼出し（`io.c:192,308`、`platform/x68k.c:69`、`sun4u.c:59`）の扱いが未記載 | `grep -rn hal_memset src/kern src/drivers` → 4 箇所 | **設計を直した。** §1・§2・§3.2・§10.1: いずれも heap 初期化後の経路なので p034 で `kern_memset` に置換する（HAL 側は変えない） |
| 14 | 低 | §9.2 の `.d` は resource dir を含まず「完全」ではない。`$(BUILD)/kern64/**/*.d` の glob は config 切替で残った古い object の `.d` を拾う。`.S` の rule は `-MMD` 無し | `vmunix.mk:280-282`（`.S` rule）と `-MMD` の system header 除外は既知 | **設計を直した。** §9: 入力を `$(AMD64_VMUNIX_OBJS:.o=.d)` の存在するものに限定し、resource dir の集合は `-M` 監査（1）が担う役割分担を明記。§6.2 の「全ヘッダが載る」を「tree 内の全ヘッダ」に訂正 |
| 15 | 低 | `plan/ws018/tests/run-legacy-bootfs-removal-host-test.sh:63` が `src/kern/entry.c` を host で compile し、p034 後は `heap.c`・`kcrt.c` が要る | `grep -rl "src/kern/entry.c" plan/*/tests/*.sh` → この 1 本 | **設計を直した。** §1・§10.1 の受入に追加（修正して PASS） |
| 16 | 低 | Vulkan 移動で更新が要るパス参照の追加: `libvulkan/README.md:6,65`、`vkdemo/README.md:21`、`dispatch-table.inc:9`（banner） | `grep -n libc/include/vulkan` で確認 | **設計を直した。** §7 の tool と文書の一覧に追加。`vulkan_wayland.h:9` は wrapper 経由で不変であることも記載 |
| 17 | 低 | 引用行番号・件数の誤り（`libc.mk:29-58`→`26-54`、`vmunix.mk:262-265`→`261-264`、`:304-306`→`303-306`、`AMD64_I915_SOURCES` は `:132`、`sys/ioctl.h` の macro は `:30-40`、`stdint.h:104-118`、`entry.c:57-58`/`:98-150`、i915-amd64 の memcmp は 29） | 各行を `grep -n` で確認（`ZEDBSD_LIBC_SOURCES :=` は 26、`AMD64_KERNEL_LIBC_OBJS :=` は 261、`AMD64_I915_SOURCES :=` は 132、`KERN_IOC_VOID` は 30、`__ZED_INT_C_PASTE` は 104、`kernel_heap_libc_lock_active[` は 57、`__libc_heap_lock(` は 109 …） | **設計を直した。** 該当箇所をすべて訂正 |

## レビューが反証しなかった主張（設計に残したもの）

- `-nostdlibinc` の探索経路（resource dir だけ）。設計担当も実際の `CC`（`--target=x86_64-unknown-zedbsd --sysroot=<p001 の sysroot>`）で
  `include src . <resource dir>` の 4 件になることを確認した（§1）。
- clang 23 の freestanding ヘッダ（`limits.h`・`stdint.h`・`stdatomic.h` が `__STDC_HOSTED__ 0` で自前定義、`UINT64_C`・
  `UINTPTR_MAX`・`offsetof`・`max_align_t`・`MB_LEN_MAX 1`）。
- 暗黙呼出しが `memcpy`・`memset` の 2 つであること、alias 実装の自己再帰が無いこと。
- kernel に `heap_allocator_realloc/calloc/aligned_alloc/reset/error_count/set_failure_after/set_grow`・`heap_active_get`・
  `heap_strdup_active`・`malloc(` の利用が無いこと（§8.1 の削除は妥当）。
- uapi が読む libc ヘッダは `sys/ioctl.h`・`sys/time.h`・`sys/types.h` の 3 種だけ。
- Vulkan を `include/uapi/vulkan/` に置く判断、`-nostdlibinc` の選択、fixture 167 script への kcrt.c 追加の却下。

## 直さなかった指摘

無し。17 件すべてに対応した。ただし次の 2 点は設計の記述を直したうえで、実装 Phase の作業として残す:

- 指摘 1 の fixture 111 ファイルへの `-DKERN_UAPI_NATIVE` の追加は p035 の作業（一覧と sed）。
- 指摘 3 の `uapi-abi-layout-check`・`posix-header-check` の rule 新設は本 WS の範囲外とし、§14.7 に判断点として残した。

## レビューが未確認と申告した点

- `/tmp/ws035-p033-probe/` の存在（設計担当の probe。レビューは自分の probe で同等結果を得た）。
- 「fixture 550 のうち 268」の再集計（規模は整合）。
- p034/p035 の見積。
- `posix-header-check` 等の rule の有無 → 設計担当が `make -n` で「rule 無し」を確認し §1・§14.7 に記録した。
