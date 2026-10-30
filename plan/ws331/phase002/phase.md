<!-- awesome-plan project=zedbsd record=ws331-p002 -->

# ws331-p002: kernel に表 B を足す（driver は変えない）

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 の表 B の B1〜B8 を include/kern と src/kern に足す。B1 は kern_device_map の宣言を変えず実装で HAL_SPACE_WC と READ だけの map を作る。B2〜B5 の errno（irq_error に HAL_ERR_STATE → EBUSY）。確かめ: 全 port の vmunix の build（warning 0）。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。

## 結果（2026-10-30、P2、worktree `agent/p2-ws331`、基点 main acd880a9a）

足した物（宣言の file と実装の file）:

| # | 足した物 | 宣言 | 実装 | 補い |
| --- | --- | --- | --- | --- |
| B1 | `KERN_DEVICE_WRITECOMBINE`（2U）・`KERN_DEVICE_READONLY`（0x100U） | include/kern/pmem.h | src/kern/pmem.c の kern_device_map | 下の 1 byte を cache の方針の値（UNCACHED→NOCACHE、WRITETHROUGH→WRITETHRU、WRITECOMBINE→WC）、READONLY は WRITE を付けない。知らない方針の値・知らない bit は EINVAL。宣言は変えない |
| B2〜B5 | `kern_irq_alloc_msi`・`kern_irq_attach_msi`・`kern_irq_detach_msi_sync`・`kern_irq_free_msi` | include/kern/irq.h | src/kern/irq.c | address は uint64_t（kern_irq_register_msi と同じ）。`irq_error` に HAL_ERR_STATE → EBUSY |
| B6 | `unsigned kern_cpu_current(void)` | include/kern/sched.h | src/kern/sched.c | |
| B7 | `void kern_console_early_putc(int character)` | include/kern/platform.h | src/kern/pmem.c | p001 の案の text-display.h・text-display.c から移した: text-display.c は HAL 無しの host 試験（plan/ws290/tests の text-end-*.sh、plan/ws014/tests/run-text-notification-test.sh）に link されるので、hal_putc を呼ぶ関数を足すと link が切れる。kern_boot_handoff（platform.h の宣言、pmem.c の実装）と同じ置き方 |
| B8 | `typedef uintptr_t kern_physaddr_t` | include/kern/pmem.h | — | `struct kern_pmem.paddr`・`kern_pmem_to_kernel()` の引数をこの型に。hal_physaddr_t と同じ表現（uintptr_t）なので、既存の呼び手・host の代役はそのまま通る |

- 動きの差: (1) 既存の `kern_device_map` の呼び手は UNCACHED（0）か WRITETHROUGH（1）だけ（grep で全数を確かめた）なので動きは変わらない。前は bit の判定（`attributes & 1`）で、値 3 などは write-through だった。今は EINVAL。(2) `irq_error` の STATE → EBUSY は、既存の kern_irq_* のうち `kern_irq_set_wake`（hal.h: suspend の間は HAL_ERR_STATE）の報告を EIO から EBUSY に変える。EIO と比べる呼び手は無い（`grep -A3 'kern_irq_…(' src` で確かめた）。
- HAL の API（include/hal/hal.h）は変えていない。driver は変えていない。pmem.h が `<hal/hal.h>` を include する件（p001 §6 の 4）は未決のまま。

### 確かめ

- `plan/ws331/tests/build-vmunix-all.sh build/p002`（20 の port の `$(BUILD)/vmunix` を 4 並列・各 make -j16、上限 30 分）: amd64・intelmac・pcat・pc98・rpi4・rpi5・odroid-c5・visionfive2・zybo・armv7-virt・riscv64-virt・riscv32-virt・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・mips32-rg350・sun4u・x68k の全部 rc=0・warning 0（log の `warning:` を数えた。kernel は -Werror）。
- 1 回目は pc98 だけ rc=2: pcat と pc98 が同時に同じ `build/i386/sysroot` を作り、pc98 の compile が header の揃う前に走った（`stddef.h` が無い）。code の誤りではない。pc98 だけを流し直して rc=0・warning 0。
- 未実施: 起動の確認（kernel の口を足しただけで呼び手は無い。p003 で起動を確かめる）、host 試験（変えた所を通す host 試験は無い）。
