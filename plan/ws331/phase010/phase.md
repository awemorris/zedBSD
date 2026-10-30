<!-- awesome-plan project=zedbsd record=ws331-p010 -->

# ws331-p010: atomic.h の HAL の分離と、pc98・pcat の legacy の窓の番地

Status: cleared（2026-10-30 Q1）
Parent: [WS331](../ws.md)
Queue: q1723（P2）

## 目的と範囲（ユーザーの決定の要点は ws.md）

1. `include/hal/atomic.h`: 契約を「macro だけ」にし、その旨を comment に書く。ENTER の port の直列化は、macro の中の statement expression の block で `hal_atomic_enter`・`hal_atomic_leave` を宣言して呼ぶ（file の scope には宣言を出さない）。`hal_atomic_cookie_t` は消し、`uintptr_t` で書く（HAL の 6 つの atomic.c の定義も合わせる。定義と macro の中の宣言の型が食い違わないよう、HAL の中の header で宣言を確かめる）。`hal/arch.h`・`hal/types.h` の 2 行の include を外し、「hal.h の中で include される事を期待し、HAL_ATOMIC_STYLE を与える側が単体で include する使い方も認める」と comment に書く。hal.h の宣言・他の契約は変えない（ユーザーの承認の範囲）。
2. `include/kern/atomic.h`: build の設定（platform の mk の -D）から `HAL_ATOMIC_STYLE` を一時的に定義して `hal/atomic.h` を include し、inline 関数を定義した後に `hal_atomic_*` の macro と `HAL_ATOMIC_STYLE` を undef する。`hal/hal.h` は include しない。
3. pc98・pcat の legacy の VRAM・VGA の窓: driver の中に straight map の基底 0x80000000 を 1 つ定義し、各番地を「基底 + 物理番地」で書く（pcat の font.c、pc98 の text.c・display-glyph.c）。display-gdc.c の基底の無い番地（0xa8000 ほか）も直す。HAL の定数は参照しない。
4. 確かめ: 20 port の build（warning 0、ENTER の port と NATIVE の port の両方）、grep で driver の TU に HAL の宣言が出ない事（kern/atomic.h の前処理の出力で確かめる）、pcat・pc98 の build と pcat の QEMU の起動、amd64・rpi4・m68k の QEMU の起動。

## 決めた形（P2、q1723）

- `include/hal/atomic.h`: `hal/arch.h`・`hal/types.h` の include を外し、`<stdint.h>` だけにした。冒頭の comment に「macro だけの契約、file scope に宣言を出さない、hal.h（arch.h の後）から読まれる事を期待し、HAL_ATOMIC_STYLE・HAL_ATOMIC_STYLE_ENTER・HAL_ATOMIC_STYLE_NATIVE を自分で与える側が単体で読んでもよい、その側は macro と include guard を一緒に undef する」と書いた。3 つが無ければ `#error`。`hal_atomic_cookie_t` を消し、ENTER の macro は statement expression（store は do-while）の block の中で `HAL_ATOMIC_ENTER_DECLARE();`（`uintptr_t hal_atomic_enter(void); void hal_atomic_leave(uintptr_t cookie)`）を宣言して呼ぶ。HAL の 6 つの atomic.c（i386・ppc32・ppc64・m68k・mips・riscv32）は定義の前に file scope で同じ `HAL_ATOMIC_ENTER_DECLARE();` を置き、定義が macro の中の宣言と食い違えば compile が落ちる（型の確かめ）。定義は `uintptr_t`。hal.h は不変。
- `include/kern/atomic.h`: `hal/hal.h` を include しない。kernel の core が先に hal.h を読んでいれば（`HAL_HAL_ATOMIC_H` が定義済み）HAL の macro をそのまま使って残す。そうでなければ、arch.h も読まれていない時に限り `HAL_ATOMIC_STYLE_NATIVE`・`_ENTER`・`HAL_ATOMIC_STYLE` を build の `-DKERN_ATOMIC_SERIALIZED` から一時的に定義し、`hal/atomic.h` を読み、inline 関数の後に `hal_atomic_*` の 12 個・`HAL_ATOMIC_ENTER_DECLARE`・include guard `HAL_HAL_ATOMIC_H`・一時の style の 3 つを undef する（後で hal.h を読む TU、例えば kern/lock.h を先に読む src/kern/lock.c は HAL の atomic.h をもう一度読む）。HAL の style を見た TU は `KERN_ATOMIC_SERIALIZED` と HAL_ATOMIC_STYLE の一致を `#error` で確かめる。
- `-DKERN_ATOMIC_SERIALIZED`: ENTER の arch（i386・m68k・mips32・ppc32・ppc64・riscv32）の `-DHAL_ARCH_*` の横に足した（Makefile の ZEDBSD_CPPFLAGS、platform の pcat・pc98（kernel・ZEDBSD_KERN_CC・dynamic libc）・m68k-virt・x68k（kernel・user）・mips32-malta（rg350 は include）・ppc32-mac99・ppc64-pseries・riscv32 の vmunix.mk）。
- 計画に無かった依存（Q1 に報告、Q1 が案 C を選んだ）: kern/atomic.h は arch/*.h の static inline の `hal_atomic_uint_try_acquire`・`hal_atomic_relax` も使っていた（driver 10 file が呼ぶ）。案 C: `atomic_raw_try_acquire_zero()`・`atomic_spin_hint()` を kern/atomic.h では宣言だけにし、本体を src/kern/lock.c（hal.h を読む kernel の core）に置いた。`atomic_try_acquire_zero()` は inline のまま前者を呼ぶ。HAL・arch の asm・意味は変えない。src/kern/test-checkpoint.c は `<hal/atomic.h>` を直に読んでいたので `<hal/hal.h>` に替えた。
- pc98: `src/drivers/platform/pc98/graphics/display.h` に `PC98_STRAIGHT_MAP_BASE`（0x80000000）を 1 つ定義し、text.c（`TEXT_VRAM_PHYSICAL` 0xa0000 から code と attribute の窓）・display-glyph.c（CG の窓 0xa4000）・display-gdc.c（plane の既定 0xa8000・0xb0000・0xb8000・0xe0000、今までは基底の無い番地で HAL の低い所の 1 対 1 の map に頼っていた）を「基底 + 物理番地」にした。backend.c は plane を kern_device_map で map し直して上書きするので、gdc の既定は map の前の値。
- pcat: `src/drivers/platform/pcat/graphics/font.c` に `PCAT_STRAIGHT_MAP_BASE`（0x80000000）と `PCAT_VGA_APERTURE_PHYSICAL`（0xa0000）を定義し、既定の `PCAT_VGA_APERTURE_ADDRESS` を和にした。amd64 は今まで通り platform/amd64/vmunix.mk の `-DPCAT_VGA_APERTURE_ADDRESS=0xffffffffc1400000ULL` で上書きする（範囲の外、変えていない）。

## 結果（2026-10-30）

- commit: ee7fcb04b（範囲 1〜3、host 試験の手直し）、この記録と追加の試験の手直しは次の commit。
- build: `plan/ws331/tests/build-vmunix-all.sh build/p010b` で 20 port（ENTER: pcat・pc98・m68k-virt・x68k・mips32-malta・mips32-rg350・ppc32-mac99・ppc64-pseries・riscv32-virt、NATIVE: 他の 11）が rc=0・warning 0。disk-image（config-pcat-boot・config-amd64-boot・config-rpi4-boot）も rc=0・warning 0（build/p010img-*）。
- driver の前処理: src/drivers/usb/usb.c を -E（kernel と同じ flag、`-P`）。amd64 は `hal_` 0 行、`HAL_` 0 行。pcat（ENTER）は `HAL_` 0 行、`hal_` は 24 行で、全てが kernel の inline 関数の中の block の宣言（`uintptr_t hal_atomic_enter(void); void hal_atomic_leave(uintptr_t cookie);`）を含む行（file scope の `hal_` の宣言は 0）。
- LTO の確かめ（Q1 の条件 1）: amd64 の vmunix に `atomic_raw_try_acquire_zero`・`atomic_spin_hint` の symbol も call も無く、pci.c の intx の取得は `drv_pci_device_establish_irq` の中に `xchgl` と `pause` の loop で inline。arm64（rpi4 の kernel.elf）は `intx_lock_enter` の中に `ldaxr`/`stxr`/`clrex` と `yield` で inline。pcat も symbol 0。戻らない port（kernel の LTO を使わない port）: m68k-virt・x68k（GCC）・ppc32-mac99・ppc64-pseries・sparcv9 では call が 1 段増える。m68k-virt の vmunix に `atomic_raw_try_acquire_zero` が T の symbol として残るのを nm で見た（m68k は HAL の `hal_atomic_uint_try_acquire` も元から out-of-line で、call が 2 段になる）。sparcv9 は NATIVE なので、spinlock の取得に call が 1 つ増える（今まで inline の asm）。Q1 は今は受け入れる（sparcv9 の件はユーザーに Q1 から）。
- QEMU（画面の PNG を boot-test が読む）: pcat bios-ide login PASS（build/p010-boot-pcat/login.png、P2 が目で見た: VGA の font で init の行と login:）。amd64 uefi-usb login PASS（build/p010-boot-amd64/login.png。最初に pcat と並べて流した 1 回は rc=1 で log が残らず、単独で流し直して PASS）。rpi4 raspi4b login PASS（build/p010-boot-rpi4/login.png）。m68k-virt は vmunix だけで `VFS initialization failed`（root が無いため、p009 と同じ）まで、scheduler・CPU 1 ONLINE を通る（build/p010-boot-m68k/login.png）。pc98 は build だけ（QEMU は範囲の外）。
- host 試験（全て PASS を自分で見た）: kernel-lock、ipv6-frag（AMD64・I386）、ws014 handle-fd（syscall の側も）・pci-service-lifecycle、ws221 packet-buf・copy-sum、ws220 tcp-model、ws290 ghci・text-end・text-end-boards（PC-98 の場面を含む）・text-end-sun4u・hal-fbwindow、ws206 gpio・firmware-smp・sdio・bcm43455・fullmac（retire を含む）、ws048 の 5 つ（dma 2・fdt・brcmstb・firmware）、ws215 rpi4-bt・bt-broadcom。
- 試験の手直し（試験の古さ、Q1 の指示で直した）:
  - この Phase で壊れた物: `atomic_raw_try_acquire_zero`・`atomic_spin_hint` の stub を足した（ipv6-frag・packet-buf・handle-fd・pci-service・tcp-model の shim・ws206 gpio・firmware-smp・ws048 firmware）。`#include "hal/atomic.h"` を `<stdint.h>` に、cookie の型を `uintptr_t` に（ipv6-frag・packet-buf）、I386 の build に `-DKERN_ATOMIC_SERIALIZED`。kern/atomic.h 経由で見えていた `hal_physaddr_t`・`bool` を `kern_physaddr_t`・`<stdbool.h>` に（ghci・ws206 gpio・firmware-smp・sdio・ws048 dma・firmware）。
  - main で既に壊れていた物: WS332 の clock の関数（`kern_ms_to_ticks`・`kern_ticks_to_ms`・`kern_us_to_ticks`・`kern_ticks_to_ns`、host の tick は 1 ms）の stub（ghci・text-end・text-end-boards・text-end-sun4u・sdio・bcm43455・fullmac・ws005 host-wlan-retire）、text-end-sun4u の HAL の名前の stub を kernel の名前に（`hal_get_arch_handoff`→`kern_boot_handoff`、`hal_putc`→`kern_console_early_putc`、`hal_irq_disable/enable`→`kern_irq_disable/enable`）、handle-fd の syscall 側の `cred_current`、ws048 brcmstb の `drv_pci_device_config_read16`・`drv_pci_device_find_capability`。
  - `run-pci-service-lifecycle-host.sh` は mktemp の directory を trap の `rm -rf` で消していたので、`build/ws014-pci-service`（第 1 引数で変えられる）に残す形にした。
  - `run-gpu-build-selection-test.py`: main でも AssertionError（amd64 で Venus=n なのに `KERN_GPU_SOURCES` が gpu.c・gpu-fence.c）。古いのは試験の方: build の規則は GPU の framework を Venus・i915・GHCI のどれかで選び（Makefile の `KERN_GPU_BACKENDS`）、arm64 は `CONFIG_DRIVER_BCM2711_GPU` でも gpu.c・gpu-fence.c を足すのに、試験は Venus だけを切り替えていた。4 つの backend を全て切り替える形に直した。直した試験と同じ make の呼び出しを手で（build/tmp/p010-gpusel/observe.mk）6 platform × n・y で流し、全ての assert の条件を満たす出力を見た。script そのものは TemporaryDirectory で消すので流していない（Q1 に頼む）。
- 注意: 手直しの前に、gpu-selection（2 回）と pci-service（1 回）は消す動作を含むまま流した（TemporaryDirectory と trap の rm -rf、/tmp の自分の一時の directory）。

## Future Work の案（登録は Q1）

- 案 A: arch/*.h の `hal_atomic_uint_try_acquire`・`hal_atomic_relax`（13 個の arch の asm）を HAL の atomic.h の macro の契約へ移し、kern/atomic.h の `atomic_raw_try_acquire_zero`・`atomic_spin_hint` を out-of-line から inline に戻す。LTO を使わない port（m68k-virt・x68k・ppc32・ppc64・sparcv9）の call の 1 段が無くなる。HAL の arch header の契約の変更なので、差分の事前承認が要る。

## 未実施・残り

- run-gpu-build-selection-test.py の実行（Q1）。
- 20 port のうち QEMU で起動を見たのは pcat・amd64・rpi4・m68k-virt。他は build だけ。pc98 の起動は見ていない（display-gdc.c・display-glyph.c・text.c の番地の変更は build と text-end-boards の host 試験の PC-98 の場面だけ）。
- 判定は Q1。
