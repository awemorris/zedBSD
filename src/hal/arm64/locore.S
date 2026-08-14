#include "defs.h"

	.section .text.boot, "ax"
	.balign 64
	.global _start
	.type _start, %function
_start:
	b arm64_boot
	nop
	.quad 0x0000000000080000
	/* Patched to the final raw-file length by check-arm64-vmunix.py. */
	.quad 0
	.quad 0
	.quad 0
	.quad 0
	.quad 0
	.long 0x644d5241
	.long 0

arm64_boot:
	msr daifset, #0xf
	mrs x1, mpidr_el1
	and x1, x1, #0xff
	cbnz x1, arm64_secondary_park
	mov x20, x0

	mrs x1, CurrentEL
	cmp x1, #(2 << 2)
	b.eq arm64_from_el2
	cmp x1, #(1 << 2)
	b.eq arm64_el1_low
	b arm64_secondary_park

arm64_from_el2:
	mov x1, #(1 << 31)
	msr hcr_el2, x1
	msr cptr_el2, xzr
	mrs x1, cnthctl_el2
	orr x1, x1, #3
	msr cnthctl_el2, x1
	msr cntvoff_el2, xzr
	mov x1, #0x3c5
	msr spsr_el2, x1
	adr x1, arm64_el1_low
	msr elr_el2, x1
	eret

arm64_el1_low:
	msr spsel, #1
	mrs x1, cpacr_el1
	orr x1, x1, #(3 << 20)
	msr cpacr_el1, x1
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

	/* Device-nGnRE mapping for 0xc0000000..0xffffffff. */
	mov x3, #0xc0000000
	add x3, x3, #0x405
	mov x4, #(3 * 8)
	adrp x1, arm64_l1_low
	add x1, x1, :lo12:arm64_l1_low
	str x3, [x1, x4]
	adrp x1, arm64_l1_high
	add x1, x1, :lo12:arm64_l1_high
	str x3, [x1, x4]

	/* Attr0=normal WB, Attr1=device-nGnRE. */
	mov x1, #0x04ff
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
