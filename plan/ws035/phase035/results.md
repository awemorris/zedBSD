# ws035-p035 結果: kernelのinclude整理と検査

Queue q318 / 項目 q318-i02。実行 2026-09-23 07:20〜（+09:00）、`/home/awe/zedBSD-rpi4`。
開始時 HEAD `1625884c`（作業ツリーclean）。**git commit はしていない。** 実行中に root が `98fe59eb`（WIP）を
commit し、その中に本Phaseが `git mv` で stage していた Vulkan ヘッダの移動（`libc/include/vulkan/` →
`include/libc/vulkan/`、内容は不変）が含まれた。それ以外の変更は未commitで作業ツリーにある。
比較の基準（baseline）は常に `1625884c`（p034完了時点）を `git archive` した木である。

## 結果の要約

- kernel・driver・HAL は libc のヘッダを1つも読まない。読むのは `include/{kern,hal,drivers,uapi,boot}`・`src/`・
  `bootloader/include/`・compiler の freestanding ヘッダ・`include/libc/vulkan/*` だけ（監査と recipe 内の検査で確認）。
- kernel の compile flags は `-nostdinc -isystem <sysroot>/usr/include` → `-nostdlibinc`。search list は
  `include src . <clang resource dir>` の4つ。sysroot を作らない木で `vmunix` が build できる。
- ABI の定義（errno・ioctl・time・stat・limits・unistd/seek・mman・wait・statvfs・un・mount を新設、types・fcntl・resource を拡張）を
  `include/uapi/` へ移し、libc の公開ヘッダは uapi を include する側にした。値・layout は移動前後で同一
  （474 項目 × 2 ABI の台帳照合 PASS）。
- Vulkan ヘッダ一式を `include/libc/vulkan/` へ移し、i915 は `<libc/vulkan/vulkan_core.h>` を読む。sysroot では従来どおり
  `/usr/include/vulkan/`（同一 hash）。
- 生成される kernel のコードは HEAD と同一（`__LINE__` の即値だけが違う。21 object・272 命令、それ以外の差 0。HAL は全 object 同一）。
- amd64・i915-amd64 の vmunix warning 0、boot-test PASS（`login:`）、userland の生成物一覧が p001 と一致。
- 未達・注意点は末尾（代表 fixture のうち directory-fsync は既存の古さで未達、boot-test の誤判定と USB の間欠 timeout）。

## 実行したコマンドと結果

### 1. 移す要素の確定と移動前の台帳（手順1）

- `python3 plan/ws035/tests/uapi-move-ledger.py`（新規）: libc 公開ヘッダ（vulkan/wayland/X11 を除く）が定義する名前と、
  kernel・driver・HAL（kcrt-rewrite の範囲＋`src/hal`・`include/hal`）が使う識別子を突き合わせ、uapi に無いものを
  ヘッダ別に列挙。移動前 `uapi-move-ledger.json`、移動後 `uapi-move-ledger-after.json`（残りは compiler の freestanding
  ヘッダの名前と、`errno`（局所変数名）・`PT_*`・`roundup` 等の kernel 自身が定義する同名の偽陽性だけ）。
- `python3 plan/ws035/tests/uapi-value-ledger.py --record plan/ws035/phase035/uapi-value-ledger-before.json --tree /tmp/p035/head`
  （新規。`/tmp/p035/head` は `1625884c` の `libc/include`・`include` の export）: `x86_64-unknown-zedbsd`（`-DKERN_USER_ABI_LP64`）と
  `i386-unknown-zedbsd` で、移すヘッダの定数 macro（値・大きさ・符号）、`include/uapi` の ioctl 番号、`P_*`、`W*`/`S_IS*` の
  評価値、型の `sizeof`、構造体の `sizeof`/`offsetof` を compiler で測って記録。474 項目。

### 2〜3. uapi の新設・拡張、libc 側の変更、uapi 内の連鎖

| uapi | 中身（libc から移したもの） | libc 側 |
| --- | --- | --- |
| `errno.h`（新） | `E*` 全部 | `errno.h` は uapi を include し `errno` と `__libc_errno_location` だけ |
| `ioctl.h`（新） | `KERN_IOC*`、`_IO/_IOR/_IOW/_IOWR` | `sys/ioctl.h` は uapi を include、`ioctl()` の宣言は残す |
| `types.h`（拡張） | `ssize_t off_t blkcnt_t blksize_t dev_t ino_t mode_t nlink_t uid_t gid_t pid_t id_t tid_t useconds_t suseconds_t reclen_t` | `sys/types.h` は `caddr_t` だけ |
| `time.h`（新） | `time_t clockid_t timer_t`、`CLOCK_MONOTONIC/REALTIME`、`TIMER_ABSTIME`、`UTIME_*`、`timespec/itimerspec/timeval/itimerval`、`ITIMER_*` | `time.h`・`sys/time.h` は uapi を include。`struct tm`・`clock_t`・関数・`timer*` macro は残す |
| `stat.h`（新） | `S_*`、`S_IS*`、`struct stat`、`st_*time`、layout の `_Static_assert` | `sys/stat.h` は関数だけ |
| `fcntl.h`（拡張） | `O_*`、`AT_*`、`FD_*`、`F_*` | `fcntl.h` は `struct flock` と関数 |
| `limits.h`（新） | `NAME_MAX PATH_MAX HOST_NAME_MAX ARG_MAX _POSIX_ARG_MAX RTSIG_MAX SIGQUEUE_MAX GETENTROPY_MAX _POSIX_{HOST_NAME,RTSIG,SIGQUEUE}_MAX SSIZE_MAX` | C の限界値と utility の限界値（`BC_*`、`LINE_MAX` 等）は残す |
| `unistd.h`（新） | `F_OK X_OK W_OK R_OK`、`STD*_FILENO`、`SEEK_SET/CUR/END/DATA/HOLE` | `unistd.h`・`stdio.h` が include |
| `mman.h`（新） | `PROT_*`、`MAP_*`（`MAP_ANON`・`MAP_FAILED` を含む）、`MS_*`、`MADV_*` | `POSIX_MADV_*` と関数は残す |
| `wait.h`（新） | `W*` macro、`WNOHANG` 等、`idtype_t`/`P_*` | 関数 |
| `statvfs.h`（新） | `fsblkcnt_t fsfilcnt_t`、`ST_*`、`struct statvfs` | 関数 |
| `un.h`（新） | `UNIX_PATH_MAX`、`struct sockaddr_un` | |
| `mount.h`（新） | `MNT_RDONLY/NOSUID/LOCAL`、`KERN_MOUNT_ARGS_VERSION`、`KERN_MOUNT_FSPEC_MAX`、`struct mount_args` | 関数 |
| `resource.h`（拡張） | `PRIO_*`、`RUSAGE_*`、`rlim_t`、`struct rlimit`、`struct rusage` | 関数 |

- 移動は文字どおり（コメントも一緒に）。唯一の綴りの変更は `SSIZE_MAX`: `LONG_MAX` → `__LONG_MAX__`（uapi が
  `<limits.h>` に依存しないため。値と型は同じで、台帳で確認）。
- uapi 内の連鎖: 13 ファイルの `<sys/ioctl.h>` → `<uapi/ioctl.h>`、`input.h`・`socket.h` の `<sys/time.h>` → `<uapi/time.h>`、
  `process.h`・`socket.h` の `<sys/types.h>` → `<uapi/types.h>`。uapi は uapi と compiler ヘッダ以外を読まない
  （host fixture 向けの分岐を除く。下記）。
- 移動後の照合 `python3 plan/ws035/tests/uapi-value-ledger.py --check plan/ws035/phase035/uapi-value-ledger-before.json`
  （`uapi-value-ledger-check.log`、`uapi-value-ledger-after.json`）: **PASS**。両 ABI で libc から見た 474 項目は全て不変、
  uapi だけ（`-nostdlibinc -Iinclude`、kernel と同じ見え方）で定義される 380 項目は全て移動前の値と一致、kernel が必要とする
  必須項目の欠落 0。

#### host fixture との共存（§6.4 の実装と、設計からの変更）

標準名の定義は `KERN_UAPI_HOST_LIBC`（`include/uapi/hosted.h`、p034 で作成）で分岐し、host fixture では host の C ライブラリの
ヘッダに委ねる。設計どおりの `#include <errno.h>` では問題が見つかったため、次の点を設計から変えた。

1. **`-I…/include/uapi` を付ける fixture（166 script）**: 新しい `uapi/errno.h`・`time.h`・`limits.h`・`unistd.h`（と既存の
   `fcntl.h`）は標準名と同名なので、`<errno.h>` がそれ自身に解決して循環する。host 分岐は `#include_next <X.h>` で次の
   ヘッダへ渡す（`-pedantic` の fixture のために直前に `#pragma GCC system_header`）。同名でないもの（`stat.h`・`mman.h` 等）は
   `<sys/X.h>` を普通に include する。
2. **KERN_UAPI_NATIVE で `-Iinclude/uapi` が `-Ilibc/include` より前の fixture**: `<limits.h>` が uapi の方に解決して
   `UINT_MAX` 等が消える。native 分岐でも、zedBSD の libc ヘッダが include path の後ろにある場合だけ（`__has_include_next(<rtld-abi.h>)`、
   rtld-abi.h は zedBSD の libc にしか無い）`#include_next` で libc の同名ヘッダへ連鎖する。zedBSD target（`__ZEDBSD__`）では
   連鎖しない（kernel・libc・userland は影響を受けない）。
3. **zedBSD の独自名**: 設計は `tid_t`・`reclen_t` 等を host でも無条件に定義するとしていたが、既存 fixture が
   `typedef uint32_t tid_t;`（`plan/ws025/tests/xhci-stream-host.c`）等を独自に定義しており衝突するため、host では定義しない
   （以前と同じ見え方）。無条件なのは `KERN_*`・`struct mount_args` 等 host に無い名前だけ。
4. **`include/kern/kcrt.h`（p034 のファイル）**: host 面の選択条件を `KERN_UAPI_HOST_LIBC` から `!defined(__ZEDBSD__) && !defined(KERN_KCRT_NATIVE)`
   へ変えた。`-DKERN_UAPI_NATIVE` を付けた fixture も host の C ライブラリを link するので、`kern_*` は host 関数への wrapper で
   なければ link できない（実際に gpu-fence 系 fixture が `kern_strcpy` 等の未定義で失敗した）。

- `sh plan/ws035/tests/run-uapi-hosted-test.sh`（新規、`uapi-hosted-test.c`）gcc・clang とも **PASS**（`uapi-hosted-test.log`、`-clang.log`）:
  host の `<errno.h>` 等と uapi を同じ TU で include して compile・実行でき host の値になる（`EINVAL=22`、host の `stat`/`clock_gettime` が動く）、
  `-Iinclude/uapi` 付きと `-pedantic` でも同じ、`-DKERN_UAPI_NATIVE` では zedBSD の値（`_Static_assert` 17 件）、
  `-Ilibc/include -DKERN_UAPI_NATIVE` で compile でき、`-Ilibc/include` で `-D` 無しは `#error "…define KERN_UAPI_NATIVE"` で止まる。

### 4. Vulkan の移動

- `git mv libc/include/vulkan include/libc/vulkan`（7 ファイル、内容不変。相対 include はそのまま解決）。
- `toolchain/llvm/sysroot.mk`: 公開ヘッダの一覧に `include/libc` を足し、`include/libc/<R>` → `usr/include/<R>`。
  sysroot の manifest で `usr/include/vulkan/*` 7 ファイルは移動前と同一 hash（`sysroot-manifest.diff` に現れない）。
- `Makefile` の sysroot 無し userland 用 `ZEDBSD_CPPFLAGS` に `-Iinclude/libc`（`<vulkan/vulkan.h>` のため）。
- 参照の更新: `userland/base/libvulkan/tools/maintain-dispatch.noct`、`dispatch-table.inc` の banner、`userland/base/libvulkan/README.md`、
  `userland/base/vkdemo/README.md`、`plan/ws014`・`plan/ws030` の試験 script・Noct 25 本の `libc/include/vulkan`。
- 生成物の一致: `noct maintain-dispatch.noct include/libc/vulkan/vulkan_core.h <protocol> <out>` で `dispatch-table.inc`・
  `api-commands.tsv`・`opcodes.h` を再生成し、3 つとも tree と `cmp` 一致。protocol 入力（virglrenderer の pinned file）は
  手元に無いため、tree の `opcodes.h` から同じ形式の定義を作って与えた（round trip。`opcodes.h` の一致はその確認で、
  pinned file からの再生成ではない）。

### 5. kernel/driver/HAL の include 行（`plan/ws035/tests/kernel-include-rewrite.py`、新規）

- 範囲は kcrt-rewrite の範囲＋`src/hal`・`include/hal`・`include/boot`（856 ファイル）。`<errno.h>` 等 → `<uapi/…>`、
  `<string.h>`・`<stdio.h>` → `<kern/kcrt.h>`（既にあれば削除。`SEEK_*` を使うファイルの `<stdio.h>` は `<uapi/unistd.h>`）、`<stdlib.h>` 等 → 削除、`<vulkan/X>` → `<libc/vulkan/X>`、
  `PATH_MAX` 等を使うファイルは `<limits.h>` の後に `<uapi/limits.h>`、HAL の `<string.h>` → `<kern/kcrt.h>`。
  引用符で書かれた標準名（`"errno.h"`、pcat/pc98 の graphics backend 2 件）も対象。
- 309 ファイル変更。集計は `include-rewrite.json`（errno 246、string 削除 221・kcrt.h へ 9、fcntl 18、types 14、vulkan 14 等）。
  当初は `<string.h>` を単に削除していたが、host fixture の回帰（9 節）で上記の規則に直して作り直した。
  `1625884c` の export で同じ script を実行した結果が作業ツリーの 309 ファイルと全て一致し、2 回目は `would_change=0`。
- HAL の差分は §8 の表の 10 行だけ（`hal.diff`: `<errno.h>`→`<uapi/errno.h>` 5、`<string.h>`→`<kern/kcrt.h>` 5）。
  HAL の object は HEAD と全て同一（下記 object 比較）。

### 6. flags・sysroot 依存・常設検査

- `platform/amd64/vmunix.mk`: `AMD64_CPPFLAGS` を `-nostdlibinc -Iinclude -Isrc -I.`、`$(AMD64_VMUNIX_OBJS)` の sysroot 依存を削除、
  make 時に `AMD64_KERNEL_SOURCES` に `libc/`・`src/libc/` の source があれば `$(error)`、`$(BUILD)/vmunix` の recipe で link の
  直後に `platform/amd64/tools/check-kernel-includes.noct $(wildcard $(AMD64_VMUNIX_OBJS:.o=.d))`（失敗なら vmunix を消す）。
- `Makefile`: `vmunix` は target の tool だけを要求し（新 `zedbsd-target-tools-ready`）、sysroot の存在確認は
  `rootfs world disk-image run` だけに残した（以前は `vmunix` も sysroot が無いと止まった）。
- `check-kernel-includes.noct`（新規）: `.d` の依存パスを正規化（`..` を解決）し、`include/{kern,hal,drivers,uapi,boot}/`、
  `include/libc/vulkan/`、`src/`、`bootloader/include/`、`plan/` 以外なら違反として列挙し非 0。
- `kernel-include-audit.py` に `--require-none` と `libc-vulkan` 分類を追加。

### 7. `-Ilibc/include` の fixture への `-DKERN_UAPI_NATIVE`（`plan/ws035/tests/add-uapi-native.py`、新規）

- `-I…libc/include` の token（shell・make・python の list）の後ろに `-DKERN_UAPI_NATIVE` を機械的に追加。108 ファイル・129 箇所
  （`native-fixtures.txt`）。`-idirafter libc/include`、`libc/include/wayland`、target build、handover/temp は対象外。
  `src/softfloat/softfloat.mk` は `$(HOSTCC)` の host 試験 4 rule だけ。2 回目は `would_change=0`。
- 追加で、p034 の `#include <kern/kcrt.h>` 挿入以来 `-I` 無しの gate が壊れていた `plan/ws004/tests/run-intel-ax211-core-test.sh` に
  `-I$repo_root/include` を足した（代表 fixture として PASS が要るため）。

### 8. build → 監査 → 台帳 → 否定試験 → userland → QEMU（各 `timeout`）

**kernel build**（`sh plan/ws035/tests/refactor-build.sh p035 <name> <config> vmunix`、JOBS=24、`timeout 1300`、最終 source、BUILD を消してから。
監査・link 検査・object 比較・disk-image・boot-test も最終 source で再実行した）:

| name | config | 結果 |
| --- | --- | --- |
| amd64 | `config/ci/config-amd64.mk` | status 0、warning 0、`kernel include check: PASS (278 objects, 7425 dependencies)`、`amd64 vmunix check: PASS`（`build/amd64.log`） |
| i915-amd64 | `plan/ws029/tests/config-i915-amd64.mk` | status 0、warning 0、`kernel include check: PASS (243 objects, 6847 dependencies)`、`vmunix check: PASS`（`build/i915-amd64.log`） |

どちらの build でも `build/ws035-p035/sysroot` は作られていない（`ZEDBSD_SYSROOT_AMD64` は存在しないパスを指したまま vmunix が
できた）。sysroot は後の userland/disk-image の build で初めて作られた。

**監査** `python3 plan/ws035/tests/kernel-include-audit.py --out plan/ws035/phase035/include-audit --require-none <name> build/<name>.log`:
両構成 **PASS**（`include-audit/amd64.json`、`i915-amd64.json`。移動前は `include-audit/before-i915-amd64.json`）。
kernel: libc 0・libc-internal 0・libc-vulkan 3（`vulkan/{vk_platform,vulkan_core,vulkan_external}.h`）・uapi 56、
compiler は `stdint.h stddef.h stdbool.h stdarg.h limits.h` と clang の `__stddef_*`・`__stdarg_*` だけ、other は `bootloader/include/` の 2 つ。
HAL: libc 0、uapi 4、other は `bootloader/include/` の 3 つ。移動前は kernel 34 種・HAL 7 種の libc ヘッダを読んでいた。
kernel が `max_align_t`・`MB_LEN_MAX` を使っていないことを grep で確認（compiler ヘッダとの差の影響なし）。

**search list**（`negative/compile-stdio.txt`）: 実在する sysroot（`build/amd64/sysroot`、`usr/include/stdio.h` あり）を `--sysroot=` に
与えても、kernel の flags での `#include <...>` の探索は `include`、`src`、`.`、`build/llvm/lib/clang/23/include` だけ。

**否定試験**（§9-5）:
- kernel の flags で `<stdio.h>`・`<string.h>`・`<errno.h>`・`<sys/types.h>`・`<vulkan/vulkan.h>` を include する TU は
  すべて `file not found` で exit 1。`<libc/vulkan/vulkan_core.h>`・`<uapi/errno.h>` は exit 0（`negative/compile-stdio.txt`）。
- `check-kernel-includes.noct` に偽の `.d` を与える（`negative/check-kernel-includes.txt`）: `include/stdio.h`（p023 後の置き場所を
  想定）→ FAIL exit 1、`libc/include/string.h`・`src/kern/../../libc/include/errno.h`・`/usr/include/stdio.h` → 3 件列挙して FAIL
  exit 1、許可されたものだけの `.d`（`include/libc/vulkan`、`bootloader/include/../../include/boot/…` を含む）→ PASS exit 0。

**libc symbol の不在**（`python3 plan/ws035/tests/kcrt-link-check.py …`、`link-check.log`）: 両構成 PASS。link 一覧の libc object 0、
baseline の libc 480 symbol のうち vmunix にあるもの 0（`memcpy`・`memset` を除く）、`llvm-nm -u kcrt.o` 空、標準名を外すと
未定義は `memcpy`・`memset` だけ。`implicit-calls.txt` 更新。

**HEAD との object 比較**（`python3 plan/ws035/tests/kernel-object-compare.py /tmp/p035/head2/build/k build/ws035-p035/amd64`、
`object-compare.txt`。前者は `1625884c` の export を同じ config・p034 の私用 sysroot で build したもの）: 282 object のうち
261 は SHF_ALLOC section が完全一致（HAL は全て一致）、21 は `.text` だけが違い、違う 272 命令は全て1つの即値が数行ずれた
`__LINE__`（fatal/assert の行番号。上の include 行を削ったため）。それ以外の差 0。

**userland**（`sh plan/ws035/tests/refactor-userland.sh p035 amd64-user plan/ws035/tests/config-amd64-userland.mk`、新規の wrapper。
p001 の `baseline-userland.sh` と同じ target 抽出を p035 の BUILD で行う）: 最終 source で BUILD を消して status 0、warning 0
（`build/amd64-user.log`）。`rootfs-bin` の前提 174 個の一覧（`build/amd64-user.targets`）は p001 の
`phase001/baseline/amd64-user.targets` と一致し、174 個とも生成された。
- 途中で userland 9 ファイルが `ioctl` の暗黙宣言で失敗した。uapi の ioctl 番号ヘッダがもう libc の `<sys/ioctl.h>`（`ioctl()` の
  宣言）を連れてこないため。`ioctl()` を呼ぶのに `<sys/ioctl.h>` を include していなかったファイルに1行ずつ足した:
  `userland/base/{mount/main.c,diskpart/main.c,mkfs/block-command.c,tests/gpu-admission/main.c,libvulkan/{context,sync,external-fence}.c}`、
  `userland/gpu/venus/{client,venus-frame}.c`。
- sysroot の manifest の `usr/include` 部分の差（`sysroot-manifest.diff`）: 変更 33（libc 16・既存の uapi 17）・追加 11（新しい uapi）・削除 0、
  Vulkan 7 ファイルは差なし。移動前側は `1625884c` の header から同じ規則で計算した。

**QEMU**:
- disk-image: `refactor-build.sh p035 amd64 config/ci/config-amd64.mk disk-image`（`DATA_IMAGE`・`SWAP_IMAGE`・`ARCH_IMAGE_DIR`・
  `ZEDBSD_IMAGE_HOST` も `build/ws035-p035/` に向けた）status 0、`build/ws035-p035/amd64/hdd-image.img` 796,917,760 byte。
  最初の完全な build では warning 4（userland 側の既存のもの: noct interpreter の `-Wreturn-type`、clang `-no-pie` 2、gmake jobserver。
  p034 と同じ）、kernel 0。最終 source での build は差分 build で warning 0。
- `OUTPUT=plan/ws035/phase035/boot-test plan/tools/boot-test.sh build/ws035-p035/amd64/hdd-image.img`（`timeout 300`）: **PASS**。
  最終 source の image（`<string.h>` 規則の修正後、08:22 build）で、画面の 30 行目が行頭の `login:`（本物の login prompt）。
  その前の image（規則修正前）でも 29 行目に `login: sshd-start: making a rsa host key` が出た。
  PNG: `plan/ws035/phase035/boot-test/login.png`、文字: `login.txt`。QEMU は TCG、実機は未確認。
- 同じ image の1回目（最終 source 前の image、kernel の命令は同じ）は、USB storage の `BOT CSW error=42`（ETIMEDOUT）が2回出て
  `/bin/login` の読込みが失敗し `getty: /bin/login: Connection timed out` で止まったが、boot-test.sh は `/bin/login:` の中の
  `login:` に一致して **PASS と誤判定した**。2回目・3回目（最終）は login prompt まで到達。`BOT CSW error=42 … tag=309` は root の
  p034 kernel の boot（`plan/tmp/boot-test/login.txt` 9 行目）にも同じく出ており、kernel の命令が HEAD と同一であることから、
  p035 の変更ではない既存の間欠的な不具合と判断した（gdbstub での解析はしていない。下の残課題）。最終 image の boot でも
  10 行目に `BOT CSW error=42 … expected-tag=308` が1回出た（その後 login まで到達）。

### 9. host fixture の回帰

`sh plan/ws035/tests/run-p034-fixture-regression.sh /tmp/p035/baseline-head plan/ws035/phase035/host-tests`（`LIMIT=300`）。
baseline は `1625884c` の `git archive`、対象は p034 と同じ 46 本（すべて guest を起動しない host fixture）。
比較は `plan/ws035/tests/fixture-error-sets.py`（新規。終了状態と正規化した error 行の集合）。

**1回目の全件実行（`<string.h>` を削除する旧規則の source、46/46 完了）**:
- 終了状態と error 集合が一致 39/46（PASS 21、両方 FAIL 18。うち ws018 legacy-bootfs など p034 と同じ既存の失敗）。
- **回帰 6 本**: `plan/ws031/tests/run-{capture,dp,lcd,lcd-modeset,native-decide,opregion}-host-test.sh` が baseline の link error
  （`drv_i915_perf_*` 未定義、既存）より前の compile error（exit 123）で止まった。原因: `display/internal.h` の `<string.h>` を
  削ったため、host では glibc の `<string.h>`（`ffs()` の宣言）が `modeset-internal.h` の `#define ffs(x)` より後に読まれ、宣言が
  macro 展開されて壊れた。**修正**: include 行の規則を「`<string.h>`・`<stdio.h>` は `<kern/kcrt.h>` に置き換える（既にあれば削除）」に
  変えて（設計 §8 の表どおり）309 ファイルを HEAD から作り直した（kernel 側で kcrt.h が新たに入ったのは `display/internal.h`・
  `render/codec.h`・`pcat/graphics/backend.c`・`kern/thread.c` の 4 つ）。修正後に `run-lcd-host-test.sh` だけを単独で再実行し、
  baseline と同じ link error（exit 1）に戻ったことを確認した。他の 5 本は同じ header を通るが、修正後の再実行はしていない。
- 状態は同じで error 集合が違ったもの 3 本: `plan/ws019/tests/run-storage-foundation-test.sh`（baseline の `O_RDWR`・`O_ACCMODE`
  未定義の compile error が消えた。uapi/fcntl.h が O_* を持つようになったため。別の既存の失敗で FAIL のまま）、
  `plan/ws004/tests/run-intel-ax211-tx-ring-test.sh`、`plan/ws002/tests/run-usb-shutdown-host.sh`（差分の内容は解析していない）。
- 1回目の log は再実行のため削除した（上の数字はその実行の出力から転記）。

**2回目（修正後の最終 source）**: root の終了指示で途中で止めた。完了したのは 1 本目の
`src/drivers/gpu/i915/tests/contracts/run.sh`（baseline・current とも PASS）だけ（`host-tests/progress-final-partial.txt`）。
**最終 source での 46 本の回帰は未実施**である。

**その他の host fixture**:
- 最終 source の前の状態で `run-gpu-fence-close-test.sh`・`run-gpu-framework-test.sh` を単独実行し PASS（kcrt.h の条件変更の確認）。
- 代表: `run-intel-ax211-core-test.sh` PASS（`representative/intel-ax211-core.log`）、`credential-creation-request-host-test.mk` PASS
  （50 checks）、`directory-fsync-host-test.mk` は既存の不整合で FAIL（受け入れ条件の表）。いずれも `<string.h>` 規則の変更前の実行。

### 10. その他

- `sh plan/ws035/tests/run-kcrt-host-tests.sh`（kcrt.h の変更後）PASS（`host-tests-kcrt.log`: kcrt 105 checks・heap 55/77 checks、通常と ASan/UBSan）。
- 置換 script の不動点: `kcrt-rewrite.py --check` 0、`kernel-include-rewrite.py --check` 0、`add-uapi-native.py --check` 0。
- `git diff --check` PASS（追跡ファイル）。新規ファイル 21 個も `git diff --no-index --check` で問題なし。いずれも最後の
  include 規則の修正（script による作り直し）より前の実行で、修正後は再実行していない。
- 規約: 新しい uapi ヘッダは規約 §13 の copyright と説明コメント、複数行コメントの形式に従った。libc から移した typedef・
  構造体・macro は「値と layout を変えない、移すだけ」に従い、元のコメント（無いものは無いまま）で移した。新しい C は
  試験用の `uapi-hosted-test.c` だけ。

## 変更の概要

| 種類 | ファイル |
| --- | --- |
| uapi 新設（11） | `include/uapi/{errno,ioctl,time,stat,limits,unistd,mman,wait,statvfs,un,mount}.h` |
| uapi 拡張・連鎖（17） | `types.h`・`fcntl.h`・`resource.h`、`<sys/ioctl.h>` 等を uapi に変えた 13 ファイル＋`input.h`・`process.h`・`socket.h` |
| libc ヘッダ（16） | `errno.h fcntl.h limits.h stdio.h time.h unistd.h sys/{ioctl,mman,mount,resource,stat,statvfs,time,types,un,wait}.h`（定義を削り uapi を include） |
| Vulkan | `libc/include/vulkan/*` → `include/libc/vulkan/*`（root の `98fe59eb` に含まれた）、`toolchain/llvm/sysroot.mk`、`Makefile` |
| kernel/driver/HAL | include 行 309 ファイル（HAL 10 行）、`include/kern/kcrt.h`（host 面の条件） |
| build | `platform/amd64/vmunix.mk`、`Makefile`（vmunix の sysroot 不要化） |
| 常設検査 | `platform/amd64/tools/check-kernel-includes.noct` |
| userland | `<sys/ioctl.h>` を足した 9 ファイル、libvulkan/vkdemo の README と `maintain-dispatch.noct`・`dispatch-table.inc` の banner |
| fixture | `-DKERN_UAPI_NATIVE` 108 ファイル、Vulkan パス 25 script、`run-intel-ax211-core-test.sh` の `-I` |
| 道具・試験（新規） | `plan/ws035/tests/{uapi-move-ledger.py,uapi-value-ledger.py,kernel-include-rewrite.py,add-uapi-native.py,kernel-object-compare.py,fixture-error-sets.py,refactor-userland.sh,run-uapi-hosted-test.sh,uapi-hosted-test.c}`、`kernel-include-audit.py` の拡張 |
| 記録 | `plan/ws035/phase035/` 以下（build log、include-audit、台帳、否定試験、object 比較、host-tests、representative、boot-test、results.md） |

## 受け入れ条件（§10.2）の達成状況

| 条件 | 状況 |
| --- | --- |
| amd64・i915-amd64 の vmunix が warning 0、`check-kernel-includes.noct` が recipe 内で PASS | **達成** |
| `clang -v` の search list に sysroot の `usr/include` が無い | **達成**（`negative/compile-stdio.txt`） |
| `kernel-include-audit.py --require-none` | **達成**（両構成。libc/libc-internal 0、other は bootloader/include だけ、compiler は許可集合内） |
| `uapi-value-ledger.py` の照合が両 triple で PASS | **達成**（474 項目、uapi 側 380 項目一致、必須欠落 0） |
| 否定試験 2 件が期待どおり失敗 | **達成** |
| sysroot 無しで kernel だけ build | **達成**（sysroot の無い BUILD で vmunix。Makefile の gate も直した） |
| userland warning 0、rootfs-bin の生成物一覧が p001 と一致、manifest の差を記録 | **達成**（userland 9 ファイルに `<sys/ioctl.h>` の追加が必要だった） |
| host fixture の回帰: p034 と同じ一覧が同じ結果 | **未確認**。旧規則の source での全件実行で ws031 の 6 本に回帰を見つけて直し、1 本で修正を確認したが、最終 source での全件の再実行は root の終了指示で途中停止（1/46 完了） |
| 代表 fixture: `directory-fsync-host-test.mk` の試験 1 本 | **未達**。`file.c` の compile は通るようになった（HEAD では `O_DSYNC` 未定義で compile 失敗）が、link で既存の不整合により失敗: 2026-09-08 に削除された `src/kern/io-stats.c` を参照し、それを外しても `file_fsync_backend` の多重定義。同系列の `credential-creation-request-host-test.mk`（`-Iinclude/uapi` と `-Ilibc/include -DKERN_UAPI_NATIVE`）は PASS（50 checks、HEAD でも PASS） |
| 代表 fixture: `-nostdinc` 付き `run-intel-ax211-core-test.sh` | **達成**（ordinary・ASan/UBSan・analyzer・amd64/i386 の `-nostdinc` syntax。HEAD では p034 以来 compile 失敗、script に `-I` を足した） |
| `uapi-hosted-test.c` で `KERN_UAPI_HOST_LIBC` の分岐を示す | **達成**（gcc・clang） |
| QEMU: boot-test | **達成**（最終 image で本物の `login:`）。ただし1回目の誤判定について下記 |
| `git diff --check`、規約 | 達成 |
| HAL の差分が §8 の表の行だけ | **達成**（`hal.diff`、10 行） |
| 未変更: HAL の宣言・実装、`include/kern/kmem.h`、libc の実装（`libc/*.c`） | **達成**（`git diff` に無い） |

## 未実施の確認

- amd64 以外の build（範囲外、壊れてよい）。arm64・GCC の platform は kernel に `-DKERN_UAPI_NATIVE` に加え `-DKERN_KCRT_NATIVE` が要る（下記）。
- i915 構成の disk-image と QEMU、実機での起動。QEMU は TCG。
- `KERN_CONSOLE_OUTPUT_TEST` の HAL build、`-DKERN_KERNEL_HEAP_TRACE` の kernel build。
- Vulkan protocol の pinned file からの `opcodes.h` 再生成（入力が手元に無く、round trip で代用）。
- `-DKERN_UAPI_NATIVE` を足した 108 fixture のうち、実行したのは回帰一覧と代表だけ。

## 残課題・人の判断が要る点

1. **`plan/tools/boot-test.sh` の誤判定**: 画面のどこかに `login:` があれば PASS にするため、`getty: /bin/login: Connection timed out`
   でも PASS になる（本 Phase の1回目で実際に起きた）。行頭の `login:` を要求するなどの修正が要る（root の道具なので直していない）。
2. **USB storage の間欠的な timeout（既存）**: `usb-storage: BOT CSW error=42` が boot 中に出て、運が悪いと `/bin/login` の読込みが
   失敗する。p034 の kernel の boot にも同じ行があり、kernel の命令は HEAD と同一なので p035 の原因ではない。gdbstub での解析は未実施。
3. **設計（§3.3・§6.4）との違い**（上記 2〜3 の節）: `kcrt.h` の host 面の条件を `__ZEDBSD__` に、同名 uapi ヘッダの委任を
   `#include_next` に、native での libc への連鎖を追加、`tid_t`/`reclen_t` を host では定義しない。これにより arm64・GCC の kernel は
   `-DKERN_UAPI_NATIVE` と `-DKERN_KCRT_NATIVE` の両方が要る（WS036）。
4. **p023 への引き継ぎ**: uapi の host 分岐は libc の include guard 名（`LIBC_ERRNO_H`、`KERN_TIME_H` 等）と `rtld-abi.h` の存在を
   目印にしている。p023 で libc ヘッダを `include/libc/` へ移すときも guard 名とファイル名を保つか、ここを直す必要がある。
   `-DKERN_UAPI_NATIVE` を足した fixture の `-I…libc/include` もパスの付け替えが要る。
5. **userland の名前空間**: `<time.h>` が uapi 経由で `struct timeval`・`ITIMER_*` と `uapi/types.h` の型を見せるようになった
   （値は不変）。userland の build は通ったが、外部 package で同名を自前定義するものがあれば衝突しうる。
   uapi の ioctl ヘッダが `ioctl()` の宣言を連れてこなくなったため、同じ依存を持つ外部コードがあれば `<sys/ioctl.h>` が要る。
6. **壊れている fixture**: `directory-fsync-host-test.mk`（`io-stats.c` の削除と多重定義。p035 以前から）。代表を探す途中で試した
   `credential-vfs-overlay-fault-host-test.mk` は古い名前 `ZEDBSD_PATH_MAX` で失敗（p035 以前の改名が原因）、
   `credential-vfs-ufs-socket-fault-host-test.mk` と ws011 `overlay-publication-host-test.mk` も失敗したが原因は調べておらず、
   HEAD での結果も取っていない。WS026 の範囲と思われるが未確認。
7. **共有 `build/`**: 回帰の fixture は既定パスで `build/` 以下に書くものがある（p034 と同じ）。開始時の `ls build/` は
   `/tmp/p035/build-before-fixtures.txt` に取ったが、終了時との比較はしていない。本 Phase は `build/ws035-p035/` 以外を削除していない。
8. **最後の include 規則の修正後に再実行していないもの**: 46 本の fixture 回帰（1/46 のみ）、ws031 display の 5 本、代表 fixture、
   gpu-fence 系の単独実行、`git diff --check`。修正は `<string.h>`/`<stdio.h>` の位置に `<kern/kcrt.h>` を置くだけで、kernel の
   build・監査・link 検査・object 比較・disk-image・boot-test は修正後に再実行して PASS。userland と uapi の台帳・hosted 試験は
   kernel の include 行に依存しない。
