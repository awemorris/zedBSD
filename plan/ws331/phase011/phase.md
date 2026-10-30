<!-- awesome-plan project=zedbsd record=ws331-p011 -->

# ws331-p011: arch の spinlock の取得と待ちの合図を HAL の atomic.h の macro の契約へ

Status: in-progress（範囲 1〜4 を実施し結果を記録、判定は Q1）
Parent: [WS331](../ws.md)
Queue: q1724（P2）

## 目的と範囲

2026-10-30 ユーザーから、hal_atomic_uint_try_acquire（と hal_atomic_relax）を include/hal/arch/atomic/<arch>.h のような file に分け、hal/atomic.h から include すれば済む、という趣旨の提案を受け、実装の指示を受けた。

1. 13 個の arch の header（include/hal/arch/*.h）の hal_atomic_uint_try_acquire・hal_atomic_relax を、include/hal/arch/atomic/<arch>.h に移す。形は「macro だけ」の契約に合わせ、asm を含む statement expression の macro にする。外の関数を呼ぶ arch（m68k など）は、macro の中の block で宣言して呼ぶ（直列化の enter・leave と同じ）。
2. hal/atomic.h は、build の -DHAL_ARCH_* で arch の file を選んで include する（hal/arch.h は include しない）。comment の契約に、この 2 つも macro として入る事を書く。arch の header（hal/arch/<arch>.h）からは消す。HAL の中で使う所（あれば）は hal/atomic.h 経由に直す。
3. kernel の atomic.h: p010 の案 C の out-of-line の関数（src/kern/lock.c の atomic_raw_try_acquire_zero・atomic_spin_hint）をやめ、HAL の macro で inline 関数に戻し、使った後に undef する。p010 で host 試験に足した stub が要らなくなれば外す。
4. 確かめ: 20 port の build（warning 0）、driver の -E に file scope の hal_ が無い事、LTO を使わない port（m68k-virt・ppc32・ppc64・sparcv9・x68k）で spinlock の取得の call が無い事（nm・逆 assemble）、QEMU pcat・amd64・rpi4・m68k の起動、p010 で流した host 試験。

## 決めた形（P2、q1724）

- `include/hal/arch/atomic/<arch>.h` を 13 個（aarch64・amd64・armv7・i386・m68030・m68040・mips32・mips64・ppc32・ppc64・riscv32・riscv64・sparcv9、arch.h の選び方と同じ名前）新しく作った。`hal_atomic_uint_try_acquire(value)` は asm を含む statement expression（引数を 1 度だけ評価して `volatile unsigned *` に受け、`hal_atomic_previous_ == 0U` を値にする。asm・制約・clobber は元の inline 関数のまま）、`hal_atomic_relax()` は asm の文。sparcv9 は元と同じ `__atomic_exchange_n` の式。file は include guard を持たず（kernel の atomic.h が macro と `HAL_HAL_ATOMIC_H` を undef した後の再読みで macro が戻るため）、`HAL_HAL_ATOMIC_H` が無ければ `#error`（hal/atomic.h 経由だけを許す）。
- m68k（m68030・m68040）: HAL の関数（src/hal/m68k/atomic.c、割り込みを止めて読む・書く）を呼ぶ。`HAL_ATOMIC_TRY_ACQUIRE_DECLARE()`（`_Bool hal_atomic_uint_try_acquire(volatile unsigned *hal_atomic_word)`）を macro の block の中で宣言して呼ぶ（自分の展開の中の同じ名前は再展開されない）。atomic.c は `#undef hal_atomic_uint_try_acquire` の後に file scope で同じ宣言の macro を置いて定義する（定義と macro の中の宣言の型の食い違いは compile で落ちる。ENTER の enter・leave と同じ形）。
- `include/hal/atomic.h`: 冒頭の契約の comment に、style の操作の他に各 port が `hal_atomic_uint_try_acquire(value)`・`hal_atomic_relax()` を `-DHAL_ARCH_*` で選んだ `<hal/arch/atomic/<arch>.h>` から与える事、関数を呼ぶ port は macro の block の中で宣言する事を足した。file の末尾で `HAL_ARCH_*` で arch の file を include する（hal/arch.h は読まない）。`HAL_ARCH_*` の無い host 試験には `__atomic_exchange_n` と compiler barrier の 2 つの macro を同じ file の `#else` に置いた（元は hal/arch.h の `#else` の static inline）。
- arch の header（13 個）と hal/arch.h の host の節から 2 つの static inline・宣言を消した。`HAL_ATOMIC_STYLE` は arch の header に残る。HAL の中の利用（i386・riscv32 の atomic.c、arm64・armv7・riscv32・riscv64・mips の spin.h、space.c・smp.c・clock.c・fwcfg.c・watch.c、amd64 の bsp-pcat/cons.c）は全て `<hal/hal.h>` を読み、hal.h が hal/atomic.h を読むので、source の変更は要らなかった。
- `include/kern/atomic.h`: `atomic_raw_try_acquire_zero()`・`atomic_spin_hint()` の宣言をやめ、p010 の前の形の static inline（`hal_atomic_uint_try_acquire`・`hal_atomic_relax` を使う）に戻した。`atomic_try_acquire_zero()` も HAL の macro を直に使う形に戻した。末尾の undef に `hal_atomic_uint_try_acquire`・`hal_atomic_relax`・`HAL_ATOMIC_TRY_ACQUIRE_DECLARE` を足した。src/kern/lock.c の 2 つの out-of-line の関数を消した。
- host 試験: p010 で足した `atomic_raw_try_acquire_zero`・`atomic_spin_hint` の stub（8 file: ipv6-frag・ws014 handle-fd・pci-service-lifecycle・ws048 firmware・ws206 firmware-smp・gpio・ws220 tcp-model の shim・ws221 packet-buf）は、static inline と衝突するので外した。

## 結果（2026-10-30）

- commit: d272f0a05（範囲 1〜3 の sparcv9 以外と host 試験の stub）、de67657dc（sparcv9 の 2 file: include/hal/arch/sparcv9.h、include/hal/arch/atomic/sparcv9.h。d272f0a05 だけでは sparcv9 の build は通らない。2 つを一緒に merge する）。この記録は次の commit。
- build: `plan/ws331/tests/build-vmunix-all.sh build/p011b` で 20 port が rc=0・warning 0。disk-image（config-pcat-boot・config-amd64-boot・config-rpi4-boot、build/p011img-*）は rc=0。amd64・rpi4 の log の warning 273 は全て perl の locale の警告（258）と OpenSSL の package の source（15）で、zedBSD の source の warning は 0（grep で分けて数えた）。
- driver の前処理: src/drivers/generic/loop.c を各 port の kernel と同じ flag で `-E -P`（20 port）。`HAL_` は全 port で 0 行。brace の深さ 0（file scope）の `hal_` は全 port で 0 行。`hal_` を含む行は NATIVE の port で 2 行（inline 関数の中の statement expression の局所変数 `hal_atomic_word_`・`hal_atomic_previous_`）、ENTER の port で 26 行（inline 関数の中の block の宣言と局所変数。m68k は `_Bool hal_atomic_uint_try_acquire(...)` の block の宣言も）、sun4u で 0 行。
- LTO を使わない port の spinlock の取得（nm と逆 assemble、build/p011b の vmunix）:
  - `atomic_raw_try_acquire_zero`・`atomic_spin_hint` の symbol は m68k-virt・x68k・ppc32-mac99・ppc64-pseries・sun4u・amd64・pcat のどれにも無い。
  - ppc32-mac99・ppc64-pseries: `spin_trylock` の中に `lwarx`/`stwcx.`/`bne-` の loop が inline（call は hal_cpu_current と、ENTER の load の hal_atomic_enter・leave だけで、これは元から）。
  - sun4u（sparcv9）: `spin_trylock` の中に `swap [%i0]` が inline。call は hal_cpu_current だけ（`spin_lock` は `spin_trylock` を呼ぶ kernel の構造で、元から）。
  - m68k-virt・x68k: `spin_trylock` から HAL の out-of-line の `hal_atomic_uint_try_acquire` へ `jsr` 1 段（m68k の HAL は元から関数。p010 の 2 段から 1 段に戻った）。
- QEMU（boot-test が画面の PNG を読む、P2 が PNG を目で見た）: pcat bios-ide login PASS（build/p011-boot-pcat/login.png）、amd64 uefi-usb login PASS（build/p011-boot-amd64/login.png）、rpi4 raspi4b login PASS（build/p011-boot-rpi4/login.png）。m68k-virt は vmunix だけで、p010 と同じく `VFS initialization failed (12)`（root が無い）まで、scheduler・`M68040 CPU 1 ONLINE`・HAL initialized を通る（build/p011-boot-m68k/login.png、rc=1 は login が無いため）。
- host 試験（全て PASS を自分で見た、build/p011-host/）: kernel-lock、ipv6-frag、ws014 handle-fd・pci-service-lifecycle、ws221 packet-buf・copy-sum、ws220 tcp-model、ws290 ghci・text-end・text-end-boards・text-end-sun4u・hal-fbwindow（14/14）、ws206 gpio・firmware-smp・sdio・bcm43455・fullmac（fullmac 2 と host-wlan-retire）、ws048 の 5 つ（dma 2・fdt・brcmstb・firmware）、ws215 rpi4-bt・bt-broadcom。gpio・firmware-smp・sdio・ws048 は既定の DTB の path（vendor/raspberrypi-firmware/…）がこの worktree に無いので、最初の 1 回は DTB が無くて落ち、rpi4 の image の build が置いた build/p011img-rpi4/bcm2711-rpi-4-b.dtb を渡して流し直して PASS。

## 未実施・残り

- 20 port のうち QEMU で起動を見たのは pcat・amd64・rpi4・m68k-virt（範囲の通り）。他は build だけ。
- run-gpu-build-selection-test.py は流していない（p010 と同じく TemporaryDirectory で消すため。この Phase では触れていない）。
- 判定は Q1。
