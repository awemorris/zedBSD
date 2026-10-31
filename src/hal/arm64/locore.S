#include "defs.h"
#include "locore-board.h"

	.section .text.boot, "ax"
	.balign 64
	.global _start
	.type _start, %function
_start:
	b arm64_boot
	nop
	.quad 0x0000000000080000
	/*
	 * The image size, from the linker script: the image once it runs, BSS
	 * included, the range a bootloader must leave free (the Linux arm64
	 * boot protocol's image_size).
	 */
	.quad __arm64_image_size
	.quad 0
	.quad 0
	.quad 0
	.quad 0
	.long 0x644d5241
	.long 0

arm64_boot:
	msr daifset, #0xf
	/* Only core 0 boots; any other core that enters here waits for ever. */
	mrs x1, mpidr_el1
	arm64_board_core_index x1
	cbnz x1, arm64_secondary_park
	mov x20, x0

	/* The board puts the image where it is linked, if the bootloader did not. */
	arm64_board_place_image

arm64_boot_placed:
	mrs x1, CurrentEL
	cmp x1, #(2 << 2)
	b.eq arm64_from_el2
	cmp x1, #(1 << 2)
	b.eq arm64_el1_low
	b arm64_secondary_park

arm64_from_el2:
	/*
	 * EL1 reads MIDR and MPIDR through VPIDR_EL2 and VMPIDR_EL2, whose
	 * reset values are unknown; they are given the real ones, so the CPU
	 * finds its own affinity at EL1 (ws208).
	 */
	mrs x1, midr_el1
	msr vpidr_el2, x1
	mrs x1, mpidr_el1
	msr vmpidr_el2, x1
	mov x1, #(1 << 31)
	msr hcr_el2, x1
	msr cptr_el2, xzr
	mrs x1, cnthctl_el2
	orr x1, x1, #3
	msr cnthctl_el2, x1
	msr cntvoff_el2, xzr
	/* EL1 starts with a known SCTLR: MMU and caches off, little-endian. */
	ldr x1, =0x30d00800
	msr sctlr_el1, x1
	mov x1, #0x3c5
	msr spsr_el2, x1
	adr x1, arm64_el1_low
	msr elr_el2, x1
	eret

arm64_el1_low:
	ldr x1, =0x30d00800
	msr sctlr_el1, x1
	isb
	ic iallu
	dsb nsh
	isb
	msr spsel, #1
	mrs x1, cpacr_el1
	orr x1, x1, #(3 << 20)
	msr cpacr_el1, x1
	isb
	/*
	 * Let EL0 read the virtual counter and its frequency for user timing.
	 * Keep timer controls and the physical counter privileged, with no
	 * event stream inherited from firmware (CNTKCTL_EL1.EL0VCTEN only).
	 */
	mov x1, #2
	msr cntkctl_el1, x1
	isb
	adrp x1, arm64_boot_stack_top
	add x1, x1, :lo12:arm64_boot_stack_top
	mov sp, x1

	/* C requires zeroed BSS; no stack memory below SP is live yet. */
	adrp x1, __bss_start
	add x1, x1, :lo12:__bss_start
	adrp x2, __bss_end
	add x2, x2, :lo12:__bss_end
1:	str xzr, [x1], #8
	cmp x1, x2
	b.lo 1b

	adrp x1, arm64_l0_low
	add x1, x1, :lo12:arm64_l0_low
	adrp x2, arm64_l1_low
	add x2, x2, :lo12:arm64_l1_low
	orr x3, x2, #3
	str x3, [x1]

	adrp x1, arm64_l0_high
	add x1, x1, :lo12:arm64_l0_high
	adrp x2, arm64_l1_high
	add x2, x2, :lo12:arm64_l1_high
	orr x3, x2, #3
	str x3, [x1]

	/* Normal WB identity/direct map for the first GiB. */
	mov x3, #0x701
	adrp x1, arm64_l1_low
	add x1, x1, :lo12:arm64_l1_low
	str x3, [x1]
	adrp x1, arm64_l1_high
	add x1, x1, :lo12:arm64_l1_high
	str x3, [x1]

	/* Device-nGnRE mapping for the gigabyte of the board's peripherals. */
	arm64_board_boot_device_block
	adrp x1, arm64_l1_low
	add x1, x1, :lo12:arm64_l1_low
	str x3, [x1, x4]
	adrp x1, arm64_l1_high
	add x1, x1, :lo12:arm64_l1_high
	str x3, [x1, x4]

	/* Any RAM past the first GiB the board's early boot touches. */
	arm64_board_boot_extra_ram

	/*
	 * Attr0=normal WB, Attr1=device-nGnRE, Attr2=device-nGnRnE,
	 * Attr3=normal non-cacheable (uncached views of RAM for DMA).
	 */
	mov x1, #0x04ff
	movk x1, #0x4400, lsl #16
	msr mair_el1, x1
	ldr x1, =0x00000002b5103510
	msr tcr_el1, x1
	adrp x1, arm64_l0_low
	add x1, x1, :lo12:arm64_l0_low
	msr ttbr0_el1, x1
	adrp x1, arm64_l0_high
	add x1, x1, :lo12:arm64_l0_high
	msr ttbr1_el1, x1
	dsb sy
	isb
	tlbi vmalle1
	dsb sy
	isb
	mrs x1, sctlr_el1
	orr x1, x1, #(1 << 0)
	orr x1, x1, #(1 << 2)
	orr x1, x1, #(1 << 12)
	/*
	 * EL0 may read CTR_EL0 (UCT), zero with DC ZVA (DZE), maintain caches
	 * by address (UCI) and run WFI and WFE (nTWI, nTWE), as on Linux.
	 */
	orr x1, x1, #(1 << 14)
	orr x1, x1, #(1 << 15)
	orr x1, x1, #(1 << 16)
	orr x1, x1, #(1 << 18)
	orr x1, x1, #(1 << 26)
	msr sctlr_el1, x1
	isb
	ldr x1, =arm64_high_entry
	br x1

arm64_high_entry:
	ldr x1, =arm64_boot_stack_top
	mov sp, x1
	mov x0, x20
	bl arm64_cmain
	b arm64_secondary_park

arm64_secondary_park:
	msr daifset, #0xf
2:	wfe
	b 2b
	.size _start, . - _start

	/*
	 * The entry of a secondary CPU the board releases (a spin table on
	 * the Pi 4, PSCI CPU_ON on the Pi 5) to this address's physical form.  It runs
	 * with the MMU off, at EL2 or EL1, and finds its own boot block by
	 * its core number: arm64_secondary_boot[core], 32 bytes each, as
	 * TTBR1 (the kernel's final tables), the stack top and the CPU number.
	 */
	.global arm64_secondary_entry
	.type arm64_secondary_entry, %function
arm64_secondary_entry:
	msr daifset, #0xf
	mrs x1, CurrentEL
	cmp x1, #(2 << 2)
	b.eq arm64_secondary_from_el2
	cmp x1, #(1 << 2)
	b.eq arm64_secondary_el1
	b arm64_secondary_park

arm64_secondary_from_el2:
	/* The same EL2 hand-off as the boot CPU's, identity registers first. */
	mrs x1, midr_el1
	msr vpidr_el2, x1
	mrs x1, mpidr_el1
	msr vmpidr_el2, x1
	mov x1, #(1 << 31)
	msr hcr_el2, x1
	msr cptr_el2, xzr
	mrs x1, cnthctl_el2
	orr x1, x1, #3
	msr cnthctl_el2, x1
	msr cntvoff_el2, xzr
	ldr x1, =0x30d00800
	msr sctlr_el1, x1
	mov x1, #0x3c5
	msr spsr_el2, x1
	adr x1, arm64_secondary_el1
	msr elr_el2, x1
	eret

arm64_secondary_el1:
	ldr x1, =0x30d00800
	msr sctlr_el1, x1
	isb
	ic iallu
	dsb nsh
	isb
	msr spsel, #1
	mrs x1, cpacr_el1
	orr x1, x1, #(3 << 20)
	msr cpacr_el1, x1
	mov x1, #2
	msr cntkctl_el1, x1
	isb

	/* Finds this CPU's boot block by its core number, at its physical address. */
	mrs x2, mpidr_el1
	arm64_board_core_index x2
	cmp x2, #4
	b.hs arm64_secondary_park
	adrp x3, arm64_secondary_boot
	add x3, x3, :lo12:arm64_secondary_boot
	add x3, x3, x2, lsl #5
	ldr x4, [x3, #0]
	ldr x19, [x3, #8]
	ldr x20, [x3, #16]
	cbz x4, arm64_secondary_park

	/*
	 * Turns the MMU on with the boot identity map in TTBR0, so the next
	 * instruction is still fetched at its physical address, and the
	 * kernel's final tables in TTBR1.
	 */
	mov x1, #0x04ff
	movk x1, #0x4400, lsl #16
	msr mair_el1, x1
	ldr x1, =0x00000002b5103510
	msr tcr_el1, x1
	adrp x1, arm64_l0_low
	add x1, x1, :lo12:arm64_l0_low
	msr ttbr0_el1, x1
	msr ttbr1_el1, x4
	dsb sy
	isb
	tlbi vmalle1
	dsb sy
	isb
	mrs x1, sctlr_el1
	orr x1, x1, #(1 << 0)
	orr x1, x1, #(1 << 2)
	orr x1, x1, #(1 << 12)
	orr x1, x1, #(1 << 14)
	orr x1, x1, #(1 << 15)
	orr x1, x1, #(1 << 16)
	orr x1, x1, #(1 << 18)
	orr x1, x1, #(1 << 26)
	msr sctlr_el1, x1
	isb
	ldr x1, =arm64_secondary_high
	br x1

arm64_secondary_high:
	/* Runs on its own stack in the kernel half, then enters C. */
	mov sp, x19
	mov x0, x20
	bl arm64_secondary_main
	b arm64_secondary_park
	.size arm64_secondary_entry, . - arm64_secondary_entry

	.section .bss.boot, "aw", %nobits
	.balign 4096
arm64_l0_low:
	.skip 4096
arm64_l1_low:
	.skip 4096
arm64_l0_high:
	.skip 4096
arm64_l1_high:
	.skip 4096
	.balign 16
arm64_boot_stack:
	.skip 16384
arm64_boot_stack_top:
