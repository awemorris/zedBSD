# ws035-p001 結果: refactorの移動対応表と影響範囲、VFIOホストの確認

Queue q316 / 項目 q316-i01。実行 2026-09-23 03:10〜03:40（+09:00）、`/home/awe/zedBSD-rpi4`、HEAD
`8df6d585c3fe5d1cc3ff84405aa979631bb5e1ad`（開始時点で作業ツリーはclean）。
**ソース・HAL・UAPI・Makefileは変更していない。** 追加したのは `plan/ws035/` 以下と、無視される
`build/ws035-p001/`（build出力と私用のtoolchain写し）だけである。git commit はしていない。

## 成果物

| 成果物 | 内容 |
| --- | --- |
| `plan/ws035/refactor-map.md` | 4つの移動の規則・判断点、HAL配下で変わる行、kernel include経路の保証案、生成された対応表（ファイル単位）と衝突表 |
| `plan/ws035/refactor-refs/` | `moves.tsv`（248ファイルの旧→新）、`refs.tsv`（全参照行）、`hal-lines.tsv`、`collisions.tsv`、`summary.json`、`run.txt` |
| `plan/ws035/tests/refactor-refs.py` | 上の2つを再生成するscript（`git ls-files` を走査、結果は決定的: 2回実行して `cmp` 一致を確認） |
| `plan/ws035/tests/kernel-include-audit.py` | kernel・HAL・kernel内libcのcompileをbuildログから `-M` で再実行し、読んだヘッダを分類。`--compare` で前後比較 |
| `plan/ws035/phase001/include-audit/*.json` | 7構成の監査結果（p023の許可リスト）と模擬2構成 |
| `plan/ws035/tests/baseline-toolchain.sh`・`baseline-build.sh`・`baseline-all.sh`・`baseline-userland.sh`・`baseline-summary.py` | refactor前後で同じ条件のbuildを行う道具 |
| `plan/ws035/tests/config-{sun4u,x68k}.mk` | `tools/menuconfig.py` の既定値から生成した構成（既存の構成が無いため） |
| `plan/ws035/tests/config-{amd64,pcat,pc98}-userland.mk` | `config/ci/` から firmware 3件（と amd64 の zedinst）を除いた構成（userland基準用） |
| `plan/ws035/phase001/baseline/` | 各buildのログ（計6.6 MB）、`*.targets`、`summary.json` |
| `plan/ws035/phase001/host-facts.json` | VFIOホストの読み取り結果とHDAの判断材料 |

## 実行したコマンドと結果

### 1. 対応表と参照一覧

`timeout 300 python3 plan/ws035/tests/refactor-refs.py`（数秒で終了、2回目の出力が1回目と一致）。

| 移動 | ファイル数 | 参照行（うちplan記録） | 編集が要る行（plan記録を除く） | HAL配下の参照行 | HAL配下で編集が要る行 |
| --- | ---: | ---: | ---: | ---: | ---: |
| p002 `include/drivers/*` → 階層化 | 38 | 649（418） | 231 | 0 | 0 |
| p003 `libc/` source → `src/libc/` | 56 | 378（245） | 116 | 0 | 0 |
| p023 `libc/include/*` → `include/*` | 148 | 6,414（219） | 137 | 23 | 0 |
| p004 boot定義 → `include/kern/boot/` | 6 | 55（16） | 37 | 6 | 6 |

- 種類別（編集が要る行）: p002 は `#include` 169・plan試験62、p003 は build 186件（Makefile/`*.mk`）・
  sysroot生成 11・plan試験 5、p023 は build 37・sysroot生成 4・Noct生成器 3・`"libc/include/..."` のinclude 7・
  plan試験 75・文書 9・Noctのpatch 2、p004 は build 24・`#include` 17（bootloader 7を含む）。
  件数は `refs.tsv` の (行, 移動対象) 単位で数えた種類別の値で、上表の「行」とは数え方が違う。
- 衝突: ファイル・ディレクトリの衝突は0。影（同じ相対名が別の `-I` にもある）は3件で、
  `include/drivers/gpu/i915/i915.h` と内部の `src/drivers/gpu/i915/i915.h`（判断点）、
  `plan/ws033/tests/include/net/{if,route}.h`（試験側が先に探索されるので影響なし）。
- `userland/base/libc/` は移動しない案とした（refactor-map.md）。

### 2. HAL配下で変わる行

p004の6行・6ファイルだけ（`src/hal/arm64/bsp-rpi4/boot.c`、`src/hal/i386/bsp-pc98/boot.c`、
`src/hal/sparcv9/bsp.h`、`src/hal/sparcv9/cmain.c`、`src/hal/x86/boot-parameters.c`、
`src/hal/x86/boot-parameters.h`）。すべて `#include` のパスの置換。`include/hal/` は0行。
p004で `include/kern/boot.h` も移す案Bを選ぶと、HALの4行が加わる。一覧は refactor-map.md と `hal-lines.tsv`。

### 3. kernelのinclude経路

`python3 plan/ws035/tests/kernel-include-audit.py <name> <log>`（7構成、各数十秒）。

| 構成 | 監査したobject | 失敗 | kernelが読むlibcヘッダ | HAL | kernel内libc | libcヘッダの経路 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| amd64 | 215 | 0 | 32 | 7 | 37 | 全て `-isystem` sysroot |
| i915-amd64 | 277 | 0 | 35 | 7 | 37 | 全て sysroot |
| pcat | 180 | 0 | 32 | 6 | 37 | kernel/HAL は sysroot、kernel内libc は `libc/include` |
| pc98 | 164 | 0 | 32 | 6 | 37 | 同上 |
| rpi4 | 148 | 0 | 32 | 4 | 37 | `-Ilibc/include` |
| sun4u | 140 | 0 | 32 | 4 | 37 | `-Ilibc/include` |
| x68k | 151 | 1 | 32 | 6 | 37 | `-Ilibc/include` |

x68k の1件は `src/hal/m68k/bsp-x68k/keyboard.c` が `"../../cons-wait.h"`（存在しない）を読むための前処理失敗で、移動前からある。
**kernelは今もlibcの公開ヘッダ（`stdio.h`、`unistd.h`、`sys/mman.h` 等）を読んでいる。** したがってp023の
保証は「読む集合と中身を変えない・増やさない」とし、案1（kernel/HALのflagsからlibc専用の経路を外し、
`-Iinclude` だけにする。監査の許可リストと `--compare`、移動後は `.d` による常設検査）を推奨した。
模擬として `ZEDBSD_TEST_CPPFLAGS=-Ilibc/include`（libcヘッダを `-I` で読む状態）で amd64・i915-amd64・pcat の
kernelをbuildし、amd64・i915 は成功（警告0）、pcat は移動前と同じlink失敗だけ、amd64の監査比較はPASSした。
これは移動後のbuildの代わりではない。

### 4. refactor前のbuildの基準

toolchain: このcheckoutには `build/` が無く、LLVMとNoctが未build。`sh plan/ws035/tests/baseline-toolchain.sh` で
`~/zedBSD/build` の LLVM 23.1.0-zedbsd6（identityで受理される）、compiler-rt builtins、Noct 2.0.1-zedbsd12 を
`build/ws035-p001/toolchain/` へ写し、sysrootも同じ場所に作った（`sysroot-amd64`・`sysroot-i386` のbuild）。
`~/zedBSD` 側へは書いていない（`make -n` で全ての書込み先が `build/ws035-p001/` 内であることを確かめてから実行）。
各buildは `sh plan/ws035/tests/baseline-build.sh <name> <config> <target>`（`make -j32`、`timeout 1800`、
専用の `BUILD=build/ws035-p001/<name>`）。userlandは `baseline-userland.sh`（`rootfs-bin` の生成物だけ。
理由は下の未実施）。失敗した構成は `EXTRA=-k` で1回ずつ全体を再buildし、失敗の全体を記録した
（同じ条件の再試行ではなく、`-k` を付けた別条件）。amd64 kernelは、sysrootが無い最初の実行が途中で止まり
objectが残ったため、`BUILD` を消してから clean build し直した（下表はclean buildの結果）。

| 構成（config） | kernel `vmunix` | userland（rootfs-binの生成物） |
| --- | --- | --- |
| amd64（`config/ci/config-amd64.mk`） | 成功、警告0、215 object、`amd64 vmunix check: PASS`、約2秒 | 成功、警告0、174 target（164 program） |
| i915-amd64（`plan/ws029/tests/config-i915-amd64.mk`、`CONFIG_DRIVER_PCI_I915 := y`） | 成功、警告0、277 object、`vmunix check: PASS` | 成功、警告0、53 target |
| pcat i386（`config/ci/config-pcat.mk`） | 失敗: link。未定義 `kern_ptrace`、`vm_device_{ref,put,read,write,page_attributes}`、`drv_pcat_serial_mirror`。compileの error・警告は0 | 失敗: `src/rtld/rtld.c:1796` unused function `static_tls_displacement`（`-Werror`）。`-k` でもこの1件 |
| pc98（`config/ci/config-pc98.mk`） | 失敗: `stage2.elf` のlink。未定義 `kern_ptrace`、`vm_device_*` 5件 | 失敗: pcatと同じ `rtld.c:1796` |
| arm64 rpi4（`config/ci/config-rpi4.mk`） | 失敗: link。未定義 `kern_ptrace` | 失敗: `clang: unsupported option '-mtls-dialect='`（aarch64）。`-k` で失敗rule 221、他に `getaddrinfo` 等の未定義 |
| sparcv9 sun4u（`plan/ws035/tests/config-sun4u.mk`） | 失敗: `src/hal/pmem-constraints.c` が無い（No rule）。`-k` では compile error 133種（`__UINT32_C_SUFFIX__` の不正suffix、不完全型など）、失敗rule 19 | 失敗: 36種（`-k` 99種）。`__UINT64_C_SUFFIX__`、`-Werror=address` 等 |
| m68k x68k（`plan/ws035/tests/config-x68k.mk`） | 失敗: sun4uと同じ No rule。`-k` で error 181種、失敗rule 20 | 失敗: 22種（`-k` 28種）、`mkfs`・`posix.c` 等 |

- 警告数は全buildで0（`-Werror` のため警告は error として数えられる）。例外はsysroot i386生成の
  clang driver警告1件（`-no-pie` 未使用）。
- `__UINT{32,64}_C_SUFFIX__` の失敗は、GCC（sparc64-elf、m68k-linux-gnu）でlibcの `stdint.h` 系のmacroが
  定義されないことによるとみられる（原因は未調査）。
- 失敗・成功の詳細（失敗rule、error例）は `plan/ws035/phase001/baseline/summary.json`。

### 5. VFIOホスト（読み取りだけ）

`timeout 60 ssh awe@10.0.10.25 ...` を3回。実行したのは `~/bigbang/igpu-mode.sh show`、`lspci -nnk`、
`readlink /sys/bus/pci/devices/*/iommu_group`、`/sys/kernel/iommu_groups/*`、`/proc/asound/*`、`reset_method`、
`systemctl is-active gdm`、`pgrep`、`sudo -n true`、設定ファイルの `cat` だけで、状態を変える操作はしていない。

- iGPU `00:02.0`（`8086:46a8` rev 0c）: `igpu-mode.sh show` = `vfio-pci`、driver `vfio-pci`、IOMMU group 0 に単独、
  reset `flr pm`。GDM inactive、QEMUは停止中。
- HDA `00:1f.3`（`8086:51c8`、Realtek ALC3254 + Alder Lake HDMI codec）: driver `snd_hda_intel`、
  **IOMMU group 15 に eSPI（LPC）bridge `00:1f.0`、SMBus `00:1f.4`（i801_smbus）、SPI controller `00:1f.5` と同居**。
  reset方法の表示なし。hostではpipewire／pipewire-pulseが動いている。
- 判断: HDAだけをVFIOで渡すことは、今のhost構成ではできない（group全体を渡す必要があり、eSPI・SPIは
  hostのplatform deviceである）。IGDとの同時パススルーも同じ理由で不可。回避策はいずれも人の判断が要る
  （`host-facts.json` の `assessment`）。

## 受け入れ条件の達成状況

| 条件 | 状況 |
| --- | --- |
| `refactor-map.md` に4つの移動の対応表がそろい、参照一覧が再生成できるscriptから出ている | 達成。対応表248行（p002 38、p003 56、p023 148、p004 6）と参照一覧を `refactor-refs.py` が生成し、2回の実行で同一 |
| HAL配下で変わる行の一覧（件数とファイル） | 達成。6行・6ファイル（すべてp004）。p002・p003・p023は0行 |
| kernelのinclude経路の保証案と検査方法 | 達成。案1〜3と検査手順、移動前の許可リスト（監査JSON）、`--compare` の動作確認（同一でPASS、別platformとの比較でFAILを検出） |
| refactor前のbuild結果（失敗は失敗のまま） | 達成。6 platform＋i915のkernelとuserland。成功はamd64とi915-amd64だけで、他は失敗として記録（修正していない） |
| `host-facts.json` があり、HDAの単独パススルーの判断材料（group構成）がそろう | 達成。group 15 の4 device、driver、reset方法 |
| ソースに差分が無い（`plan/` 以外の変更なし） | 達成。`git status --short` は `plan/` 配下の未追跡ファイルだけ（`plan/ws034/phase001/` は並行するws034-p001のもの）。`build/` は `.gitignore` 対象 |

## 未実施の確認

- rootfsの組み立て（`rootfs-bin` 本体、`rootfs`、`disk-image`）。`rootfs-bin` は固定パス
  `build/llvm/share/licenses/llvm/LICENSE.TXT` を前提に要求し、このcheckoutに作ると管理外の `build/llvm` が
  できてしまう。firmware programは `git.kernel.org` から共有の `build/sources` へ取得する。このため
  userlandは `rootfs-bin` の生成物（`$(BUILD)` 配下のprogram・library）だけをbuildし、firmware 3件と
  amd64の `zedinst` を構成から除いた。packages（`rootfs-usr`）もbuildしていない。
- QEMU・実機での起動は範囲外で、行っていない。
- 移動後の実buildは行っていない（p023の模擬は flags を足した移動前の木でのbuildである）。
- host試験（`plan/*/tests/` のhost test）の実行はしていない。参照一覧に数えただけ。

## 残課題・人の判断が要る点

1. **HALの承認**: p004の6行（案Bなら10行）の `#include` 置換。p023ではHALのsourceは変わらないが、
   HALのcompile flags（`platform/*/vmunix.mk` の `HAL_CC`・`*_CPPFLAGS`）と、HALが読む23行のlibcヘッダの
   探索先（sysroot／`libc/include` → `include/`）が変わる。これを承認の対象に含めるかの判断が要る。
2. **HDA（p008）**: 単独パススルー不可。QEMU intel-hdaだけにする、ベアメタル起動で確かめる、group 15
   全体を渡す、USB audioを使う、のどれにするかは人の判断（WS035の未決事項1）。
3. **p002の判断点**: `ws.md` の例（xHCIを `usb/xhci/`）と `src/drivers/` の実配置（`pci/`）の食い違い、
   `hid/`・`graphics/` の置き先、`i915.h` の同名問題、buildされない `i915-old/` の17行を触るか。
4. **p004**: 既存の `include/kern/boot.h` を残す（案A、推奨）か移す（案B）か。
5. **前提の訂正**: `ws.md` p003の「kernelのbuildがuserland用ヘッダを誤って拾う恐れ」について、kernelは今も
   32個のlibc公開ヘッダを読んでいる（実測）。p023の保証は「集合と中身を変えない」とした。kernelのlibc依存を
   減らすかは別の設計判断。
6. **計画に無い依存（p002以降に影響）**:
   - このcheckoutにはtoolchainが無い。refactorの各Phaseのbuildも、`baseline-toolchain.sh` の私用の写し
     （`~/zedBSD/build` から）か、正式な `make toolchain` が要る。
   - 「全platformのbuildで確かめる」（ws.md p002）は、今はamd64でしかbuildが通らない。他のplatformは
     「移動前と同じ失敗」で回帰を判定するしかない。pcat・pc98・rpi4 のkernel link失敗（`kern_ptrace`、
     `vm_device_*` 未定義）、sun4u・x68k の `src/hal/pmem-constraints.c` 欠落、userlandの `rtld.c` の
     `-Werror`、aarch64の `-mtls-dialect=`、GCCでの `__UINT*_C_SUFFIX__` は、refactorとは別の既存の不具合で、
     担当WS／Bugへの登録が要る（このPhaseでは直していない）。
   - p023でNoctのpatch（`userland/base/noct/patches/0001`・`0002`）を変えると、Noctのpatch levelとsource抽出に
     波及する。
