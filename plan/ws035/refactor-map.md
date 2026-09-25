# WS035 refactor: 移動の対応表と影響範囲

ws035-p001 の成果物。p002 → p003 → p023 → p004 の順に行う4つの移動について、ファイル単位の対応表と、
各移動対象を参照する行の一覧（件数・ファイル）をまとめる。**このPhaseではソースを動かしていない。**

- 対応表と参照一覧は `python3 plan/ws035/tests/refactor-refs.py` が作る（再生成できる）。
  出力は `plan/ws035/refactor-refs/`（`moves.tsv`、`refs.tsv`、`hal-lines.tsv`、`collisions.tsv`、`summary.json`）と、
  この文書末尾の生成節。git の追跡ファイル全体（`git ls-files`）を走査する。
- 各refactor Phaseの完了後に同じscriptを再実行する。移動済みのファイルは規則に掛からなくなり、
  残りのPhaseの対象と参照が現在の木で数え直される（例えば p023 の数は p003 完了後に変わる）。
- 「参照行」は1行につき1件（同じ行が複数の移動対象を名指しすれば `refs.tsv` では対象ごとに1行）。
  「編集が要る」は、移動後にその綴りでは同じファイルに届かなくなる行。`#include` は移動後の
  ファイル集合で探索を模擬して判定し（`-Iinclude`、`-Isrc`、`-I.`、`-Iinclude/libc` の順、`"..."` は
  そのファイルのディレクトリを先に見る）、新しい綴りを元の書き方（ディレクトリ相対／root相対）のまま出す。
  Makefile等の文字列はパスとして名指ししていれば数える（`userland/base/libc/`、`bootloader/include/` は別の木なので数えない）。
- `plan-records` は `plan/` の記録（tests以外）。過去の記録なので書き換えない前提で、編集数から除く。

## 規則と判断

### p002: `include/drivers/*` → `src/drivers/` と同じ階層

- 規則: ヘッダを実装するsource（または唯一のkernel利用者）がある `src/drivers/<dir>/` と同じ
  `include/drivers/<dir>/` へ移す。**basenameは変えない**（機械的な移動に留める）。根拠の列に実装側を書いた。
- 判断が要る点（p002の着手時に決める）:
  1. `ws.md` の例は `pci-xhci.h` → `include/drivers/usb/xhci/...` だが、`src/drivers/` では xHCI は
     `src/drivers/pci/pci-xhci.c` にある。対応表は規則どおり `include/drivers/pci/` とした。
  2. `include/drivers/hid/`、`include/drivers/graphics/` に対応する `src/drivers/hid/`・`graphics/` は無い。
     実装側に合わせ `ps2-8042.h` → `platform/pcat/`、`pc98-busmouse.h` → `platform/pc98/`、
     `hid-report.h` → `usb/`（kernelの利用者は `usb-hid.c` だけ）、`graphics/pc98.h`・`pcat.h` →
     `platform/<機種>/graphics/` とした。p005（`/dev/graphics` 共通層）で graphics は再び動く可能性がある。
  3. `i915.h` → `include/drivers/gpu/i915/i915.h` は、内部ヘッダ `src/drivers/gpu/i915/i915.h` と
     `drivers/gpu/i915/i915.h` という同じ相対名を持つ（衝突表の shadow）。現在のi915内部は `"i915.h"`・
     `"../i915.h"` の相対includeなので解決は変わらず、`<drivers/gpu/i915/i915.h>` は `-Iinclude` が先に
     見つかるので動作はする。ただし紛らわしいので、basenameを保つか改名（例 `i915-driver.h`）するかを決める。
  4. `src/drivers/gpu/i915-old/`（buildされない参照コピー、WS035では触らない）に編集が要る行が17行ある。
     触らずに古いincludeのまま残すか、機械置換だけ行うかを決める。
- HAL配下で変わる行: 0。

### p003: `libc/` のsource → `src/libc/`

- 規則: `include/libc/` 以外の `libc/` の全ファイル（`libc.mk`、`regex/` と `regex/LICENSE`、内部ヘッダ
  `heap.h`・`locale-db.h`・`stdio-internal.h`、`setjmp-aarch64.S` を含む）を、相対配置を保って `src/libc/` へ移す。
  `include/libc/` はp023まで元の場所に残す（p003とp023の間、`libc/` には `include/` だけが残る）。
- 影響の中心はbuild: `include libc/libc.mk`、source一覧（`libc/heap.c ...`）、object名
  （`$(BUILD)/libc/*.o`、`kern64/libc/`、`dynamic/obj/libc/`）、pattern rule、`sysroot.mk` のsource一覧。
- `#include "libc/heap.h"` 等（14行）は、今は `-I.` で解決し、移動後は `-Isrc` で同じ綴りのまま解決する
  （`keep-root .->src`）。`-Isrc` を持たないcompile（host試験など）があれば綴りか flags の変更が要る。
- `userland/base/libc/`（`posix.c` 等のユーザー空間実行時）は**移動しない**。ユーザー指定の範囲は
  トップレベルの `libc/` であり、`userland/base/libc/` は別の木（ユーザー専用のsyscall層・pthread等）である。
- HAL配下で変わる行: 0。

### p023: `include/libc/*` → `include/*`

- 規則: `include/libc/<R>` → `include/<R>`（148ファイル、`X11/`、`vulkan/`、`wayland/`、`linux/`、`machine/`、
  `sys/`、`net/` 等のサブディレクトリを含む）。
- 名前の衝突: `include/` の既存の最上位（`boot/`、`drivers/`、`hal/`、`kern/`、`uapi/`）と、libcヘッダの
  最上位名は重ならない。ファイル単位の衝突も無い（生成節の衝突表）。
- 影の検査: build fileに現れる全ての `-I` ディレクトリについて、`<R>` と同じ相対名のファイルを探した。
  該当は `plan/ws033/tests/include/net/{if,route}.h` だけで、そのhost試験は `-Iplan/ws033/tests/include` を
  `-Iinclude` より前に置くので、移動後も試験用ヘッダが優先される（影響なし）。
- `#include <stdio.h>` などは綴りが変わらない（6,052行が `keep-root include/libc->include`）。変わるのは
  `-Iinclude/libc`・`-idirafter include/libc` 等のflags、`sysroot.mk` の見出しmanifest、`"include/libc/..."` と
  書いた7行のinclude、Noctの生成器と文書である。
- 注意（p023の計画に入れる）:
  1. `toolchain/llvm/sysroot.mk` は `find include/libc include/uapi` でsysrootへ入れるヘッダを選ぶ。移動後は
     `include/` の中から kernel用（`kern/`、`hal/`、`drivers/`、`boot/`）を除いた集合をmanifestにする必要がある。
     sysrootの `usr/include` の中身（manifestのSHA-256）が移動前と一致することを受け入れ条件にする。
  2. `userland/base/noct/patches/0001-*.patch`・`0002-*.patch` が `include/libc` を名指しする。patchを変えると
     Noctのpatch levelとsource抽出（`ZEDBSD_NOCT_PATCH_LEVEL`）に波及する。
  3. `userland/base/libvulkan/tools/maintain-dispatch.noct` がheaderを読み、生成物 `dispatch-table.inc` の
     コメントにパスを書く。p023では再生成して一致を確かめる（ws030の再生成試験）。
- HAL配下で変わる行: 0（HALのsourceが読む23行の `<stdint.h>` 等は綴りが変わらず、探索rootだけが
  sysroot／`include/libc` から `include/` に変わる）。HALのcompile flagsは `platform/*/vmunix.mk` にあり、
  HAL配下のファイルではない。

### p004: boot定義 → `include/kern/`

- 規則: `include/kern/*`（4ファイル）→ `include/kern/*`。`include/kern/boot.h` →
  `include/kern/boot.h`、`include/kern/boot.h` → `include/kern/boot.h`（同じ場所で
  どちらも `boot.h` になるため、機種名をファイル名にする）。空になる `include/kern/`、`include/kern/rpi4/`、
  `include/kern/sun4u/` は消える。include guardは変えない（機械的な移動）。
- `bootloader/` は `"../../include/kern/boot.h"` や `-I.` からの `"include/kern/..."` で参照しているので、
  bootloader側の綴りも変わる（生成表の c-include）。
- **既存の `include/kern/boot.h` の扱い（案）**:
  - 案A（推奨）: そのまま残す。`include/kern/boot.h`（ファイル）と `include/kern/`（ディレクトリ）は
    同じ親に並べられ、`<kern/boot.h>` と `<kern/boot.h>` は区別される。追加の編集は0行。
    `kern/boot.h` は kernel側のboot API（`kern_boot_parameters_*`、stage2/handoff構造体）で、
    `rpi4.h`・`sun4u.h` はその `struct kern_boot_handoff` を拡張する側なので、親子関係も読み取れる。
  - 案B: `include/kern/boot.h`（または `handoff.h`）へ移す。`#include <kern/boot.h>` の29行
    （HAL 4行、kernel 7行、include 6行、bootloader 1行、plan 11行）が変わり、HALの承認対象が4行増える。
  どちらでも機能は同じなので、ユーザーの好みで決める。決まらなければ案Aで進める。
- HAL配下で変わる行: 6行（6ファイル、下の一覧）。案Bなら +4行。

## HAL配下で変わる行（承認を求める材料）

`plan/ws035/refactor-refs/hal-lines.tsv` の、綴りが変わる行だけを抜き出した。すべて `#include` の
パスの機械的な置換で、他の変更は無い。p002・p003・p023ではHAL配下の変更は0行。

| Phase | ファイル | 行 | 変更 |
| --- | --- | ---: | --- |
| p004 | `src/hal/arm64/bsp-rpi4/boot.c` | 3 | `<kern/boot.h>` → `<kern/boot.h>` |
| p004 | `src/hal/i386/bsp-pc98/boot.c` | 12 | `<kern/boot.h>` → `<kern/boot/pc98-handoff.h>` |
| p004 | `src/hal/sparcv9/bsp.h` | 4 | `<kern/boot.h>` → `<kern/boot.h>` |
| p004 | `src/hal/sparcv9/cmain.c` | 4 | `<kern/boot.h>` → `<kern/boot.h>` |
| p004 | `src/hal/x86/boot-parameters.c` | 13 | `<kern/boot.h>` → `<kern/boot/pc98-handoff.h>` |
| p004 | `src/hal/x86/boot-parameters.h` | 16 | `<kern/boot.h>` → `<kern/boot.h>` |

合計 6行・6ファイル（すべてp004）。`include/hal/` 配下は0行。p004の案Bを選ぶと、
`<kern/boot.h>` を読むHALの4行が加わる。

## kernelのinclude経路の保証（p023の設計案）

### 現状（実測）

`plan/ws035/tests/kernel-include-audit.py` で、baseline buildのkernel・HAL・kernel内libcのcompileを
`-M` で再実行し、実際に読まれたヘッダを数えた（`plan/ws035/phase001/include-audit/*.json`）。

- kernelは**今もlibcの公開ヘッダを読んでいる**。`src/kern`・`src/drivers` が32個（i915構成は35個。
  `stdio.h`、`stdlib.h`、`unistd.h`、`fcntl.h`、`sys/mman.h`、`sys/socket.h`、`termios.h` 等を含む）、
  HALが4〜7個（`stdint.h`、`stddef.h`、`errno.h`、`string.h` 等）、kernelへ組み込むlibc（`libc/*.c`）が37個。
- 経路はplatformで違う。amd64のkernel/HALと、pcat・pc98のkernel/HALは `-isystem <sysroot>/usr/include`
  （sysrootは `include/libc` の写し）から、arm64・sparcv9・x68kは `-Iinclude/libc` から、pcat・pc98の
  kernel内libcは `libc/libc.mk` の `-Iinclude/libc` から読む。
- `-MMD` は `-isystem` 経由のヘッダを依存に書かないので、amd64・pcat・pc98の `.d` だけでは
  kernelがどのlibcヘッダを読んだかは分からない（このため `-M` で再実行する監査を作った）。

したがって「kernelがlibcのヘッダを拾わない」は現状の性質ではない。p023で保証すべきことは、
**kernel・HALが読むlibcヘッダの集合と中身が移動の前後で変わらず、新しいlibcヘッダを誤って拾わない**ことである。
kernelが使うlibcヘッダを減らす（kernel専用の最小ヘッダへ分ける）かは別の設計判断で、p023の範囲外とする。

### 案

- 案1（推奨、最小）: kernel・HALのflagsから `-Iinclude/libc` と `-isystem <sysroot>/usr/include` を外し、
  libcヘッダも `-Iinclude` だけで見つかるようにする。移動後は `include/<R>` が非system headerとして読まれる
  ので、`-MMD` の `.d` に全てのlibcヘッダが載り、依存の漏れも無くなる。これに次の検査を組み合わせる。
  - 許可リスト: 移動前の監査結果（object class ごとのlibcヘッダ集合、`include-audit/*.json`）を固定する。
  - 検査: p023完了時に全platformで `kernel-include-audit.py` を再実行し、`--compare` で移動前と一致すること
    （`libc`、`libc-internal`、`uapi`、`other` の集合）。p023以降の常設検査としては、kernel objectの
    `.d` を読み、許可リストに無い `include/<libcの最上位名>/...` が現れたら失敗するmake targetを作る
    （aggregate `make check` ではなく、kernel buildの後に走る個別target）。
  - 警告: libcヘッダが `-isystem` でなくなると、そのヘッダ内の警告が `-Werror` で止まり得る。
    **模擬試験**として、移動前の木で `ZEDBSD_TEST_CPPFLAGS=-Iinclude/libc` を付け、amd64（既定・i915）と
    pcatのkernelを「libcヘッダを `-I` で読む」状態でbuildした。amd64・i915は成功し警告0、pcatは
    移動前と同じlink失敗だけで新しい警告・errorは無く、監査の比較もPASSした（`*-sim-p023.log`）。
    ただし実際の移動後のbuildの代わりではない。
- 案2（強い隔離）: kernel用のヘッダ見取り図 `$(BUILD)/kernel-include/` を生成し、`kern/`・`hal/`・`drivers/`・
  `boot/`・`uapi/` と、許可リストのlibcヘッダだけをsymlinkで並べて、kernel・HALは `-Iinclude` の代わりに
  これを使う。許可されていないlibcヘッダはcompile時に見つからずerrorになる。生成規則と許可リストの
  保守が増える。
- 案3（補助）: 利用者空間専用のディレクトリ（`X11/`、`vulkan/`、`wayland*`、`dlfcn.h`、`pthread.h` 等）に
  kernel buildで定義されるmacroを見て `#error` するguardを置く。libcヘッダの編集になるので、p023の
  機械的な移動とは別に扱う。

推奨は案1。案2は、案1の検査で漏れが実際に見つかった場合に検討する。

### 検査方法（再掲）

1. 移動前: `plan/ws035/tests/baseline-all.sh vmunix` と `-k` 付きのbuildログを作り、
   `kernel-include-audit.py <platform> <log>` で `include-audit/<platform>.json` を作る（このPhaseで実施済み）。
2. 移動後: 同じ構成で build し、同じscriptで `<platform>-after.json` を作り、
   `kernel-include-audit.py --compare include-audit/<platform>.json <platform>-after.json` がPASSすること。
3. sysroot: `<sysroot>/.zedbsd-sysroot-manifest` の `usr/include/` 部分が移動前と同一であること。

## refactor前のbuildの基準

`plan/ws035/phase001/results.md` の「refactor前のbuild」を正とする。要約: amd64（既定・i915）のkernelと
userlandは成功、pcat・pc98・arm64 rpi4はkernelのlinkで失敗、sun4u・x68kは存在しないsourceの規則で
kernelが止まり、userlandはpcat・pc98・rpi4・sun4u・x68kで失敗する（いずれも移動前から）。
refactor後は、同じscriptで同じ構成をbuildし、成功・失敗と失敗の内容（`summary.json` の
`failed_rules`・`error_samples`、パスは移動に合わせて読み替える）が一致することを回帰の基準にする。

<!-- refactor-refs:start -->

（この節は `python3 plan/ws035/tests/refactor-refs.py` が生成する。手で編集しない。）

### p002: `include/drivers/*` → `src/drivers/` と同じ階層

移動 0 ファイル。参照 0 行（0 ファイル、うちplan記録 0 行）、編集が要る行 0（plan記録を除く）。HAL配下 0 行（編集が要る行 0）。

| 旧パス | 新パス | 参照行（plan記録を除く） | うち編集 | plan記録 | 根拠 |
| --- | --- | ---: | ---: | ---: | --- |

移動済み 38 ファイル。旧パスを指す行 876、うちplan記録・`i915-old/`・このscriptを除く 1（一覧は `stale.tsv`）。

| 旧パス | 新パス | 根拠 |
| --- | --- | --- |
| `include/drivers/disklabel.h` | `include/drivers/disklabel/disklabel.h` | src/drivers/disklabel/*.c |
| `include/drivers/dma.h` | `include/drivers/generic/dma.h` | src/drivers/generic/dma.c |
| `include/drivers/dp8390.h` | `include/drivers/ethernet/dp8390.h` | src/drivers/ethernet/dp8390.c |
| `include/drivers/gpu-display.h` | `include/drivers/gpu/gpu-display.h` | src/drivers/gpu/gpu.c |
| `include/drivers/gpu-fence.h` | `include/drivers/gpu/gpu-fence.h` | src/drivers/gpu/gpu-fence.c |
| `include/drivers/gpu-scanout.h` | `include/drivers/gpu/gpu-scanout.h` | src/drivers/gpu/gpu.c |
| `include/drivers/gpu-share.h` | `include/drivers/gpu/gpu-share.h` | src/drivers/gpu/gpu.c |
| `include/drivers/gpu.h` | `include/drivers/gpu/gpu.h` | src/drivers/gpu/gpu.c |
| `include/drivers/graphics/pc98.h` | `include/drivers/platform/pc98/graphics/pc98.h` | src/drivers/platform/pc98/graphics/pc98-graphics.c |
| `include/drivers/graphics/pcat.h` | `include/drivers/platform/pcat/graphics/pcat.h` | src/drivers/platform/pcat/graphics/pcat-graphics.c |
| `include/drivers/hid/hid-report.h` | `include/drivers/usb/hid-report.h` | src/drivers/usb/usb-hid.c (only kernel user) |
| `include/drivers/hid/pc98-busmouse.h` | `include/drivers/platform/pc98/pc98-busmouse.h` | src/drivers/platform/pc98/pc98-busmouse.c |
| `include/drivers/hid/ps2-8042.h` | `include/drivers/platform/pcat/ps2-8042.h` | src/drivers/platform/pcat/ps2-8042.c |
| `include/drivers/i915.h` | `include/drivers/pci/pci-i915.h` | src/drivers/gpu/i915/ (PCI registration) |
| `include/drivers/pc98-lgy98.h` | `include/drivers/platform/pc98/pc98-lgy98.h` | src/drivers/platform/pc98/pc98-lgy98.c |
| `include/drivers/pcat-ide.h` | `include/drivers/platform/pcat/pcat-ide.h` | src/drivers/platform/pcat/pcat-ide.c |
| `include/drivers/pcat-ne2000.h` | `include/drivers/isa/pcat-ne2000.h` | src/drivers/isa/ne2000.c |
| `include/drivers/pci-ehci.h` | `include/drivers/pci/pci-ehci.h` | src/drivers/pci/pci-ehci.c |
| `include/drivers/pci-intel-ax211.h` | `include/drivers/wifi/intel-ax211/pci-intel-ax211.h` | src/drivers/wifi/intel-ax211/intel-ax211.c |
| `include/drivers/pci-nvme-protocol.h` | `include/drivers/pci/pci-nvme-protocol.h` | src/drivers/pci/pci-nvme.c |
| `include/drivers/pci-nvme.h` | `include/drivers/pci/pci-nvme.h` | src/drivers/pci/pci-nvme.c |
| `include/drivers/pci-pcat.h` | `include/drivers/pci/pci-pcat.h` | src/drivers/pci/pci-pcat.c |
| `include/drivers/pci-uhci.h` | `include/drivers/pci/pci-uhci.h` | src/drivers/pci/pci-uhci.c |
| `include/drivers/pci-xhci-capability.h` | `include/drivers/pci/pci-xhci-capability.h` | src/drivers/pci/pci-xhci.c |
| `include/drivers/pci-xhci-control.h` | `include/drivers/pci/pci-xhci-control.h` | src/drivers/pci/pci-xhci.c |
| `include/drivers/pci-xhci-lifecycle.h` | `include/drivers/pci/pci-xhci-lifecycle.h` | src/drivers/pci/pci-xhci.c |
| `include/drivers/pci-xhci.h` | `include/drivers/pci/pci-xhci.h` | src/drivers/pci/pci-xhci.c |
| `include/drivers/pci.h` | `include/drivers/pci/pci.h` | src/drivers/pci/pci.c |
| `include/drivers/usb-cdc-ecm.h` | `include/drivers/usb/usb-cdc-ecm.h` | src/drivers/usb/usb-cdc-ecm.c |
| `include/drivers/usb-cdc-ncm.h` | `include/drivers/usb/usb-cdc-ncm.h` | src/drivers/usb/usb-cdc-ncm.c |
| `include/drivers/usb-hid.h` | `include/drivers/usb/usb-hid.h` | src/drivers/usb/usb-hid.c |
| `include/drivers/usb-rtl8822bu.h` | `include/drivers/usb/usb-rtl8822bu.h` | src/drivers/usb/usb-rtl8822bu.c |
| `include/drivers/usb-storage-bot.h` | `include/drivers/usb/usb-storage-bot.h` | src/drivers/usb/usb-storage.c |
| `include/drivers/usb-storage-scsi.h` | `include/drivers/usb/usb-storage-scsi.h` | src/drivers/usb/usb-storage.c |
| `include/drivers/usb-storage.h` | `include/drivers/usb/usb-storage.h` | src/drivers/usb/usb-storage.c |
| `include/drivers/usb-uas.h` | `include/drivers/usb/usb-uas.h` | src/drivers/usb/usb-uas*.c |
| `include/drivers/usb.h` | `include/drivers/usb/usb.h` | src/drivers/usb/usb.c |
| `include/drivers/venus.h` | `include/drivers/pci/pci-venus.h` | src/drivers/gpu/venus/venus.c (PCI registration) |

### p003: `libc/` のsource → `src/libc/`

移動 56 ファイル。参照 8494 行（172 ファイル、うちplan記録 8340 行）、編集が要る行 139（plan記録を除く）。HAL配下 0 行（編集が要る行 0）。

| 旧パス | 新パス | 参照行（plan記録を除く） | うち編集 | plan記録 | 根拠 |
| --- | --- | ---: | ---: | ---: | --- |
| `libc/catalog.c` | `src/libc/catalog.c` | 1 | 1 | 36 |  |
| `libc/ctype.c` | `src/libc/ctype.c` | 11 | 11 | 93 |  |
| `libc/err.c` | `src/libc/err.c` | 2 | 2 | 48 |  |
| `libc/fenv.c` | `src/libc/fenv.c` | 10 | 10 | 53 |  |
| `libc/float-parse.c` | `src/libc/float-parse.c` | 9 | 9 | 73 |  |
| `libc/fmtmsg.c` | `src/libc/fmtmsg.c` | 1 | 1 | 32 |  |
| `libc/fnmatch.c` | `src/libc/fnmatch.c` | 1 | 1 | 30 |  |
| `libc/format.c` | `src/libc/format.c` | 11 | 11 | 79 |  |
| `libc/ftw.c` | `src/libc/ftw.c` | 2 | 2 | 50 |  |
| `libc/heap.c` | `src/libc/heap.c` | 15 | 14 | 107 |  |
| `libc/heap.h` | `src/libc/heap.h` | 5 | 0 | 36 |  |
| `libc/int64.c` | `src/libc/int64.c` | 11 | 11 | 90 |  |
| `libc/inttypes.c` | `src/libc/inttypes.c` | 2 | 2 | 46 |  |
| `libc/libc.mk` | `src/libc/libc.mk` | 1 | 1 | 71 |  |
| `libc/libgen.c` | `src/libc/libgen.c` | 2 | 2 | 47 |  |
| `libc/libutil.c` | `src/libc/libutil.c` | 0 | 0 | 15 |  |
| `libc/locale-db.c` | `src/libc/locale-db.c` | 1 | 1 | 36 |  |
| `libc/locale-db.h` | `src/libc/locale-db.h` | 2 | 0 | 29 |  |
| `libc/locale.c` | `src/libc/locale.c` | 11 | 11 | 107 |  |
| `libc/math.c` | `src/libc/math.c` | 9 | 9 | 122 |  |
| `libc/ndbm.c` | `src/libc/ndbm.c` | 2 | 2 | 52 |  |
| `libc/openbsd-base64.c` | `src/libc/openbsd-base64.c` | 1 | 1 | 25 |  |
| `libc/openbsd-digest.c` | `src/libc/openbsd-digest.c` | 1 | 1 | 26 |  |
| `libc/openbsd-glob.c` | `src/libc/openbsd-glob.c` | 1 | 1 | 31 |  |
| `libc/openbsd-opts.c` | `src/libc/openbsd-opts.c` | 1 | 1 | 29 |  |
| `libc/openbsd-sha2.c` | `src/libc/openbsd-sha2.c` | 1 | 1 | 24 |  |
| `libc/openbsd-vis.c` | `src/libc/openbsd-vis.c` | 1 | 1 | 30 |  |
| `libc/openbsd.c` | `src/libc/openbsd.c` | 1 | 1 | 40 |  |
| `libc/random.c` | `src/libc/random.c` | 2 | 2 | 46 |  |
| `libc/random48.c` | `src/libc/random48.c` | 2 | 2 | 45 |  |
| `libc/readpassphrase.c` | `src/libc/readpassphrase.c` | 1 | 1 | 31 |  |
| `libc/realpath.c` | `src/libc/realpath.c` | 2 | 2 | 50 |  |
| `libc/regex/LICENSE` | `src/libc/regex/LICENSE` | 0 | 0 | 2 |  |
| `libc/regex/regcomp.c` | `src/libc/regex/regcomp.c` | 1 | 1 | 49 |  |
| `libc/regex/regerror.c` | `src/libc/regex/regerror.c` | 1 | 1 | 44 |  |
| `libc/regex/regexec.c` | `src/libc/regex/regexec.c` | 1 | 1 | 50 |  |
| `libc/regex/tre-mem.c` | `src/libc/regex/tre-mem.c` | 1 | 1 | 44 |  |
| `libc/regex/tre.h` | `src/libc/regex/tre.h` | 3 | 0 | 19 |  |
| `libc/search.c` | `src/libc/search.c` | 2 | 2 | 49 |  |
| `libc/setjmp-aarch64.S` | `src/libc/setjmp-aarch64.S` | 1 | 1 | 6 |  |
| `libc/setjmp.c` | `src/libc/setjmp.c` | 2 | 2 | 45 |  |
| `libc/stdio-extra.c` | `src/libc/stdio-extra.c` | 2 | 2 | 53 |  |
| `libc/stdio-internal.h` | `src/libc/stdio-internal.h` | 4 | 0 | 19 |  |
| `libc/stdio.c` | `src/libc/stdio.c` | 11 | 11 | 77 |  |
| `libc/stdlib-extra.c` | `src/libc/stdlib-extra.c` | 2 | 2 | 52 |  |
| `libc/string-extra.c` | `src/libc/string-extra.c` | 2 | 2 | 51 |  |
| `libc/string.c` | `src/libc/string.c` | 14 | 14 | 104 |  |
| `libc/strto.c` | `src/libc/strto.c` | 11 | 11 | 88 |  |
| `libc/syslog.c` | `src/libc/syslog.c` | 1 | 1 | 34 |  |
| `libc/sysv-ipc.c` | `src/libc/sysv-ipc.c` | 1 | 1 | 69 |  |
| `libc/tempnam.c` | `src/libc/tempnam.c` | 2 | 2 | 50 |  |
| `libc/time-extra.c` | `src/libc/time-extra.c` | 2 | 2 | 49 |  |
| `libc/wide-extra.c` | `src/libc/wide-extra.c` | 2 | 2 | 62 |  |
| `libc/wide.c` | `src/libc/wide.c` | 11 | 11 | 97 |  |
| `libc/xsi-crypto.c` | `src/libc/xsi-crypto.c` | 2 | 2 | 47 |  |
| `libc/xsi-process.c` | `src/libc/xsi-process.c` | 1 | 1 | 40 |  |

ディレクトリ単位の参照（`-Iinclude/libc`、`libc/%.c` など）:

| 参照先 | 参照行（plan記録を除く） | plan記録 |
| --- | ---: | ---: |
| `libc/` | 38 | 6265 |
| `libc/regex/` | 1 | 7 |

### p023: `include/libc/*` → `include/*`

移動 139 ファイル。参照 15790 行（1798 ファイル、うちplan記録 10008 行）、編集が要る行 263（plan記録を除く）。HAL配下 13 行（編集が要る行 0）。

| 旧パス | 新パス | 参照行（plan記録を除く） | うち編集 | plan記録 | 根拠 |
| --- | --- | ---: | ---: | ---: | --- |
| `include/libc/X11/X.h` | `include/X11/X.h` | 1 | 0 | 4 |  |
| `include/libc/X11/Xlib.h` | `include/X11/Xlib.h` | 5 | 0 | 8 |  |
| `include/libc/X11/Xzed.h` | `include/X11/Xzed.h` | 4 | 0 | 7 |  |
| `include/libc/X11/keysym.h` | `include/X11/keysym.h` | 2 | 0 | 4 |  |
| `include/libc/aio.h` | `include/aio.h` | 2 | 0 | 8 |  |
| `include/libc/ar.h` | `include/ar.h` | 0 | 0 | 2 |  |
| `include/libc/arpa/inet.h` | `include/arpa/inet.h` | 17 | 1 | 21 |  |
| `include/libc/assert.h` | `include/assert.h` | 184 | 0 | 186 |  |
| `include/libc/atomic-compiler.h` | `include/atomic-compiler.h` | 1 | 0 | 6 |  |
| `include/libc/catalog-format.h` | `include/catalog-format.h` | 2 | 2 | 5 |  |
| `include/libc/crypt.h` | `include/crypt.h` | 3 | 0 | 5 |  |
| `include/libc/ctype.h` | `include/ctype.h` | 34 | 0 | 37 |  |
| `include/libc/curses.h` | `include/curses.h` | 2 | 1 | 6 |  |
| `include/libc/dev/evdev/input-event-codes.h` | `include/dev/evdev/input-event-codes.h` | 0 | 0 | 2 |  |
| `include/libc/dev/evdev/input.h` | `include/dev/evdev/input.h` | 1 | 0 | 3 |  |
| `include/libc/devctl.h` | `include/devctl.h` | 1 | 0 | 4 |  |
| `include/libc/dirent.h` | `include/dirent.h` | 36 | 0 | 41 |  |
| `include/libc/dlfcn.h` | `include/dlfcn.h` | 5 | 0 | 7 |  |
| `include/libc/endian.h` | `include/endian.h` | 2 | 0 | 5 |  |
| `include/libc/err.h` | `include/err.h` | 1 | 0 | 5 |  |
| `include/libc/errno.h` | `include/errno.h` | 547 | 2 | 808 |  |
| `include/libc/fcntl.h` | `include/fcntl.h` | 217 | 1 | 237 |  |
| `include/libc/features.h` | `include/features.h` | 6 | 0 | 9 |  |
| `include/libc/fenv.h` | `include/fenv.h` | 5 | 0 | 7 |  |
| `include/libc/float.h` | `include/float.h` | 0 | 0 | 2 |  |
| `include/libc/fmtmsg.h` | `include/fmtmsg.h` | 1 | 0 | 3 |  |
| `include/libc/fnmatch.h` | `include/fnmatch.h` | 4 | 2 | 6 |  |
| `include/libc/ftw.h` | `include/ftw.h` | 1 | 0 | 5 |  |
| `include/libc/getopt.h` | `include/getopt.h` | 2 | 0 | 5 |  |
| `include/libc/glob.h` | `include/glob.h` | 2 | 0 | 8 |  |
| `include/libc/grp.h` | `include/grp.h` | 13 | 0 | 17 |  |
| `include/libc/inttypes.h` | `include/inttypes.h` | 7 | 0 | 11 |  |
| `include/libc/langinfo.h` | `include/langinfo.h` | 3 | 0 | 7 |  |
| `include/libc/libgen.h` | `include/libgen.h` | 1 | 0 | 3 |  |
| `include/libc/libintl.h` | `include/libintl.h` | 2 | 0 | 6 |  |
| `include/libc/limits.h` | `include/limits.h` | 115 | 0 | 112 |  |
| `include/libc/link.h` | `include/link.h` | 2 | 0 | 9 |  |
| `include/libc/locale-format.h` | `include/locale-format.h` | 4 | 2 | 7 |  |
| `include/libc/locale.h` | `include/locale.h` | 19 | 0 | 27 |  |
| `include/libc/machine/endian.h` | `include/machine/endian.h` | 0 | 0 | 3 |  |
| `include/libc/machine/reg.h` | `include/machine/reg.h` | 0 | 0 | 2 |  |
| `include/libc/math.h` | `include/math.h` | 6 | 0 | 8 |  |
| `include/libc/md5.h` | `include/md5.h` | 2 | 0 | 6 |  |
| `include/libc/mqueue.h` | `include/mqueue.h` | 3 | 0 | 8 |  |
| `include/libc/ndbm.h` | `include/ndbm.h` | 1 | 0 | 4 |  |
| `include/libc/net/ethernet.h` | `include/net/ethernet.h` | 0 | 0 | 2 |  |
| `include/libc/net/if.h` | `include/net/if.h` | 10 | 1 | 12 |  |
| `include/libc/net/route.h` | `include/net/route.h` | 7 | 1 | 9 |  |
| `include/libc/netdb.h` | `include/netdb.h` | 12 | 0 | 16 |  |
| `include/libc/netinet/in.h` | `include/netinet/in.h` | 17 | 1 | 19 |  |
| `include/libc/netinet/in_systm.h` | `include/netinet/in_systm.h` | 1 | 0 | 4 |  |
| `include/libc/netinet/ip.h` | `include/netinet/ip.h` | 0 | 0 | 5 |  |
| `include/libc/netinet/tcp.h` | `include/netinet/tcp.h` | 2 | 0 | 6 |  |
| `include/libc/netpacket/packet.h` | `include/netpacket/packet.h` | 0 | 0 | 2 |  |
| `include/libc/nl_types.h` | `include/nl_types.h` | 2 | 1 | 5 |  |
| `include/libc/paths.h` | `include/paths.h` | 0 | 0 | 3 |  |
| `include/libc/poll.h` | `include/poll.h` | 34 | 0 | 39 |  |
| `include/libc/pthread.h` | `include/pthread.h` | 51 | 0 | 59 |  |
| `include/libc/pty.h` | `include/pty.h` | 7 | 0 | 10 |  |
| `include/libc/pwd.h` | `include/pwd.h` | 16 | 0 | 20 |  |
| `include/libc/readpassphrase.h` | `include/readpassphrase.h` | 2 | 0 | 5 |  |
| `include/libc/regex.h` | `include/regex.h` | 7 | 0 | 10 |  |
| `include/libc/resolv.h` | `include/resolv.h` | 4 | 0 | 11 |  |
| `include/libc/rtld-abi.h` | `include/rtld-abi.h` | 5 | 0 | 10 |  |
| `include/libc/sched.h` | `include/sched.h` | 19 | 0 | 22 |  |
| `include/libc/search.h` | `include/search.h` | 1 | 0 | 4 |  |
| `include/libc/semaphore.h` | `include/semaphore.h` | 3 | 0 | 7 |  |
| `include/libc/setjmp.h` | `include/setjmp.h` | 4 | 0 | 8 |  |
| `include/libc/sha1.h` | `include/sha1.h` | 2 | 0 | 6 |  |
| `include/libc/sha2.h` | `include/sha2.h` | 2 | 0 | 6 |  |
| `include/libc/shadow.h` | `include/shadow.h` | 2 | 0 | 5 |  |
| `include/libc/signal.h` | `include/signal.h` | 58 | 0 | 63 |  |
| `include/libc/spawn.h` | `include/spawn.h` | 4 | 0 | 9 |  |
| `include/libc/stdalign.h` | `include/stdalign.h` | 0 | 0 | 2 |  |
| `include/libc/stdarg.h` | `include/stdarg.h` | 81 | 0 | 81 |  |
| `include/libc/stdatomic.h` | `include/stdatomic.h` | 11 | 0 | 18 |  |
| `include/libc/stdbool.h` | `include/stdbool.h` | 26 | 0 | 29 |  |
| `include/libc/stddef.h` | `include/stddef.h` | 343 | 1 | 349 |  |
| `include/libc/stdint.h` | `include/stdint.h` | 774 | 2 | 1468 |  |
| `include/libc/stdio.h` | `include/stdio.h` | 618 | 0 | 629 |  |
| `include/libc/stdlib.h` | `include/stdlib.h` | 427 | 0 | 434 |  |
| `include/libc/stdnoreturn.h` | `include/stdnoreturn.h` | 0 | 0 | 2 |  |
| `include/libc/string.h` | `include/string.h` | 676 | 0 | 917 |  |
| `include/libc/strings.h` | `include/strings.h` | 7 | 0 | 10 |  |
| `include/libc/sys/file.h` | `include/sys/file.h` | 2 | 0 | 6 |  |
| `include/libc/sys/ioctl.h` | `include/sys/ioctl.h` | 78 | 6 | 81 |  |
| `include/libc/sys/ipc.h` | `include/sys/ipc.h` | 6 | 0 | 9 |  |
| `include/libc/sys/mman.h` | `include/sys/mman.h` | 33 | 0 | 39 |  |
| `include/libc/sys/mount.h` | `include/sys/mount.h` | 9 | 0 | 14 |  |
| `include/libc/sys/msg.h` | `include/sys/msg.h` | 5 | 0 | 9 |  |
| `include/libc/sys/param.h` | `include/sys/param.h` | 0 | 0 | 5 |  |
| `include/libc/sys/ptrace.h` | `include/sys/ptrace.h` | 1 | 0 | 5 |  |
| `include/libc/sys/queue.h` | `include/sys/queue.h` | 1 | 0 | 3 |  |
| `include/libc/sys/quota.h` | `include/sys/quota.h` | 2 | 0 | 4 |  |
| `include/libc/sys/random.h` | `include/sys/random.h` | 2 | 0 | 6 |  |
| `include/libc/sys/resource.h` | `include/sys/resource.h` | 11 | 0 | 17 |  |
| `include/libc/sys/select.h` | `include/sys/select.h` | 1 | 0 | 7 |  |
| `include/libc/sys/sem.h` | `include/sys/sem.h` | 5 | 0 | 9 |  |
| `include/libc/sys/shm.h` | `include/sys/shm.h` | 5 | 0 | 9 |  |
| `include/libc/sys/snapshot.h` | `include/sys/snapshot.h` | 3 | 0 | 5 |  |
| `include/libc/sys/socket.h` | `include/sys/socket.h` | 60 | 2 | 63 |  |
| `include/libc/sys/stat.h` | `include/sys/stat.h` | 116 | 0 | 122 |  |
| `include/libc/sys/statvfs.h` | `include/sys/statvfs.h` | 7 | 0 | 15 |  |
| `include/libc/sys/sysctl.h` | `include/sys/sysctl.h` | 13 | 0 | 17 |  |
| `include/libc/sys/time.h` | `include/sys/time.h` | 18 | 0 | 24 |  |
| `include/libc/sys/times.h` | `include/sys/times.h` | 3 | 0 | 6 |  |
| `include/libc/sys/tree.h` | `include/sys/tree.h` | 1 | 0 | 3 |  |
| `include/libc/sys/types.h` | `include/sys/types.h` | 78 | 0 | 96 |  |
| `include/libc/sys/uio.h` | `include/sys/uio.h` | 14 | 0 | 18 |  |
| `include/libc/sys/un.h` | `include/sys/un.h` | 18 | 1 | 21 |  |
| `include/libc/sys/utsname.h` | `include/sys/utsname.h` | 3 | 0 | 5 |  |
| `include/libc/sys/wait.h` | `include/sys/wait.h` | 58 | 0 | 63 |  |
| `include/libc/sys/xattr.h` | `include/sys/xattr.h` | 1 | 0 | 5 |  |
| `include/libc/sysexits.h` | `include/sysexits.h` | 0 | 0 | 2 |  |
| `include/libc/syslog.h` | `include/syslog.h` | 2 | 0 | 5 |  |
| `include/libc/terminfo.h` | `include/terminfo.h` | 2 | 0 | 6 |  |
| `include/libc/termios.h` | `include/termios.h` | 20 | 0 | 24 |  |
| `include/libc/threads.h` | `include/threads.h` | 4 | 0 | 8 |  |
| `include/libc/time.h` | `include/time.h` | 104 | 0 | 109 |  |
| `include/libc/uchar.h` | `include/uchar.h` | 1 | 0 | 6 |  |
| `include/libc/ulimit.h` | `include/ulimit.h` | 1 | 0 | 3 |  |
| `include/libc/unistd.h` | `include/unistd.h` | 303 | 0 | 312 |  |
| `include/libc/util.h` | `include/util.h` | 1 | 0 | 5 |  |
| `include/libc/utmpx.h` | `include/utmpx.h` | 6 | 0 | 11 |  |
| `include/libc/vis.h` | `include/vis.h` | 2 | 0 | 5 |  |
| `include/libc/wayland-client-core.h` | `include/wayland-client-core.h` | 1 | 1 | 7 |  |
| `include/libc/wayland-client-protocol.h` | `include/wayland-client-protocol.h` | 1 | 1 | 7 |  |
| `include/libc/wayland-client.h` | `include/wayland-client.h` | 5 | 1 | 11 |  |
| `include/libc/wayland-util.h` | `include/wayland-util.h` | 1 | 1 | 7 |  |
| `include/libc/wayland/API-PROVENANCE.md` | `include/wayland/API-PROVENANCE.md` | 0 | 0 | 6 |  |
| `include/libc/wayland/wayland-client-core.h` | `include/wayland/wayland-client-core.h` | 5 | 0 | 12 |  |
| `include/libc/wayland/wayland-client-protocol.h` | `include/wayland/wayland-client-protocol.h` | 4 | 0 | 11 |  |
| `include/libc/wayland/wayland-client.h` | `include/wayland/wayland-client.h` | 3 | 0 | 11 |  |
| `include/libc/wayland/wayland-util.h` | `include/wayland/wayland-util.h` | 2 | 0 | 10 |  |
| `include/libc/wayland/xdg-shell-client-protocol.h` | `include/wayland/xdg-shell-client-protocol.h` | 2 | 0 | 10 |  |
| `include/libc/wayland/zed-gpu-buffer-v1-client-protocol.h` | `include/wayland/zed-gpu-buffer-v1-client-protocol.h` | 3 | 0 | 11 |  |
| `include/libc/wchar.h` | `include/wchar.h` | 14 | 0 | 22 |  |
| `include/libc/wctype.h` | `include/wctype.h` | 5 | 0 | 9 |  |
| `include/libc/xdg-shell-client-protocol.h` | `include/xdg-shell-client-protocol.h` | 4 | 1 | 9 |  |

ディレクトリ単位の参照（`-Iinclude/libc`、`libc/%.c` など）:

| 参照先 | 参照行（plan記録を除く） | plan記録 |
| --- | ---: | ---: |
| `include/libc/` | 237 | 2886 |

### p004: boot定義 → `include/kern/`

移動 6 ファイル。参照 249 行（62 ファイル、うちplan記録 195 行）、編集が要る行 52（plan記録を除く）。HAL配下 6 行（編集が要る行 6）。

| 旧パス | 新パス | 参照行（plan記録を除く） | うち編集 | plan記録 | 根拠 |
| --- | --- | ---: | ---: | ---: | --- |
| `include/kern/boot.h` | `include/kern/boot.h` | 15 | 14 | 51 |  |
| `include/kern/boot.h` | `include/kern/boot.h` | 19 | 18 | 61 |  |
| `include/kern/boot.h` | `include/kern/pc98-handoff.h` | 9 | 9 | 28 |  |
| `include/kern/boot.h` | `include/kern/boot.h` | 5 | 5 | 37 |  |
| `include/kern/boot.h` | `include/kern/boot.h` | 3 | 3 | 11 |  |
| `include/kern/boot.h` | `include/kern/boot.h` | 5 | 5 | 14 |  |

ディレクトリ単位の参照（`-Iinclude/libc`、`libc/%.c` など）:

| 参照先 | 参照行（plan記録を除く） | plan記録 |
| --- | ---: | ---: |
| `include/kern/` | 7 | 18 |
| `include/kern/rpi4/` | 1 | 3 |
| `include/kern/sun4u/` | 1 | 2 |

### 衝突・影の検査

| Phase | 種類 | パス | 内容 |
| --- | --- | --- | --- |
| p023 | shadow | `include/net/if.h` | <net/if.h> also exists as plan/ws033/tests/include/net/if.h (-Iplan/ws033/tests/include); -Iinclude is searched first |
| p023 | shadow | `include/net/route.h` | <net/route.h> also exists as plan/ws033/tests/include/net/route.h (-Iplan/ws033/tests/include); -Iinclude is searched first |
| p004 | name-beside | `include/kern/` | include/kern/boot.h stays a file beside the new directory (legal; see refactor-map.md) |

<!-- refactor-refs:end -->
