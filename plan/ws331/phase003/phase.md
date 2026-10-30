<!-- awesome-plan project=zedbsd record=ws331-p003 -->

# ws331-p003: 表 A の機械的な置き換え

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 §2 の表 A と §5 の p003 の行のとおり、driver の hal_* と HAL の型・定数を kern_* に、include だけの file から HAL の include を外す。sun4u の 2 file は sparcv9（ユーザーが main で手を入れている）なので別の commit。確かめ: 各 port の build（warning 0）と、変えた driver を含む port の起動の確認（boot-test.sh）。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。

## 結果（2026-10-30、P2、worktree `agent/p2-ws331`）

commit（sparcv9 は分けた）:

| SHA | 内容 |
| --- | --- |
| b7e70b3be | sun4u と i915 以外の 34 file: rtc_read_counter・get_arch_handoff・irq_disable/enable・io/mmio・HAL_FATAL→KERN_FATAL・putc→kern_console_early_putc・cpu_current→kern_cpu_current・`(hal_physaddr_t)`→`(kern_physaddr_t)`、acpi-kern.c・typec-kern.c の map を kern_device_map（UNCACHED、acpi の読み出しの page は UNCACHED\|READONLY）と errno、include だけの file の `hal/hal.h` を外す、変えた行の近くの comment の `hal_*` |
| fae1fcd11 | sparcv9: sun4u-console.c・sun4u-firmware-fb.c（同じ置き換え）。main 888701726 で両 file に acd880a9a からの変更・未 commit の変更が無い事を確かめた |
| c956a7615 | 続き（Q1 の指示、p001 の一覧の漏れ）: `kernel_putc` の宣言を include/kern/text-display.h に足し、x68k-console.c の `hal/hal.h` を外す。USB の 9 file（pci-ehci・pci-uhci・pci-xhci・usb-ccid・usb-hid・usb-hid-checkpoint・usb-hub・usb-storage・usb-uas-disk）の `kernel_notify_task` を `sched_notify_task`（include/kern/sched.h の既存の関数）に |
| 6f31f7e03 | 続きの sparcv9: sun4u-console.c の `hal/hal.h` を外す |

- i915 は全部 p004 に回した。pc98-lgy98.c・pc98/graphics/backend.c・JIS の表（p005）と rpi4-console.c の cons.h（p005）は触っていない。
- 見つけた漏れ（p001 の grep は `hal_` で始まる名前だけを数えた）: `kernel_putc`（hal.h でだけ宣言された kernel の変数、console の driver 6 file が読み書き）と `kernel_notify_task`（hal.h の HAL 向けの wrapper、USB の 9 file）。前者は kernel の header の宣言で、後者は既存の `sched_notify_task` で置き換えた（2026-10-30 ユーザーの決定: wrapper の新しい名前は作らず sched の名前を使う、putc の宣言は可）。
- `sched_notify_task` の呼び方: 呼び手は attach・detach・停止の thread の文脈と、完了・port の変化の通知（割り込みの文脈を含む）。`sched_notify_task` は CPU の lock を `spin_lock_irqsave` で取り、眠らず、別の CPU には `hal_cpu_notify` を送るだけなので、どちらの文脈からも呼べる。`kernel_notify_task` はこれを呼ぶだけの関数だったので、動きは変わらない。
- 残る問題（未決の「kernel の header が HAL の header を include する件」と同じ）: `sched_notify_task(hal_task_t)` の引数の型は HAL の型のまま。driver の `worker->task` も `hal_task_t`。kernel の型を作るかは未決の件と合わせて決める。
- build の成功は HAL の名前を使っていない証拠にならない（kern の header が `hal/hal.h` を include するので、driver が HAL の宣言を間接に使っても通る。acpi-kern.c の `*value = hal_io_inp8(...)` の行を置き換えの script が comment と見て残し、build が通った事で分かった）。確かめは grep で行った。

### 確かめ

- build: `plan/ws331/tests/build-vmunix-all.sh build/p003`・`build/p003b`（c956a7615・6f31f7e03 の後）で 20 の port の vmunix が全部 rc=0・warning 0。
- grep: `grep -rnE '\bhal_|\bHAL_|hal/|\bpaddr_t|kernel_notify_task' src/drivers include/drivers`（i915 を除く）の残りは、pc98-lgy98.c・pc98/graphics/backend.c の include、JIS の表（`hal_pc98_jisx0208_to_ucs`、p005）、rpi4-console.c の `hal/arm64/bsp-rpi-common/cons.h`（p005）、build の macro `HAL_ARCH_*`・`HAL_BOARD_*`（未決）だけ。
- QEMU の起動（`plan/tools/boot-test.sh`、画面の PNG を読んで確かめた。image の config は `plan/ws331/tests/config-{amd64,pcat,rpi4,sun4u}-boot.mk`、login に要る program だけで text の login）:

| port | 版 | mode | 結果 |
| --- | --- | --- | --- |
| pcat（i386） | fae1fcd11 | bios-ide | login PASS |
| sun4u | fae1fcd11 | sun4u | login PASS |
| rpi4 | fae1fcd11 | raspi4b | login PASS |
| riscv64-virt | b7e70b3be | vmunix | `VFS initialization failed` まで PASS（ns16550 の serial が attach） |
| armv7-virt | b7e70b3be | vmunix | `ARMV7 HAL READY …` PASS |
| amd64 | 6f31f7e03（p004 を含む） | uefi-nvme | login PASS（i915 は QEMU に無いので i915 の実行は通らない） |
| pcat（i386） | 6f31f7e03 | bios-ide | login PASS（kernel_putc・sched_notify_task の後） |
| sun4u | 6f31f7e03 | sun4u | login PASS（同上） |

- 未実施: pc98 の起動（`plan/tools/pc98-boot.py` は PC-98 fork の QEMU が要る、試していない）、x68k（build だけ）、zybo（QEMU の zynq は生きた serial の対話が要る）、visionfive2・odroid-c5・rpi5（QEMU の起動の道具が無い／実機）。
