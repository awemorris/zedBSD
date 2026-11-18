/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS kernel's first instructions.
 *
 * The loader enters at the image's linked KSEG0 address in kernel mode,
 * with Status.BEV set, so that an exception would go to the boot ROM's
 * vectors and nothing would report it.  The first step therefore clears
 * Cause, whose IV bit a loader may leave set, points EBase at the image's
 * own vectors and clears BEV (and ERL, which a loader may also leave
 * set), before any instruction that may fault.  Then Status is set to
 * its kernel value (on the 64-bit processor with KX, so that the kernel
 * may reach xkphys, and UX, so that the user's 64-bit segment takes the
 * 64-bit TLB refill), the board marks the first stage of the boot,
 * KSEG0 is made cacheable, the bss is cleared and C is entered on the
 * boot stack with the loader's four arguments as they came: the argument
 * count, the argument table, the environment table and the size of low
 * memory.
 */

#include "asm.h"

	.set noreorder
	.set noat

	.section .text.boot, "ax"
	.balign 4
	.global _start
	.type _start, %function
_start:
	/* Clears Cause, so that interrupts come to the general exception vector. */
	mtc0 $0, MIPS_CP0_CAUSE
	ehb

	/* Points the exceptions at the image's vectors, then leaves the boot vectors. */
	LOAD_ADDRESS($8, mips_vectors)
	mtc0 $8, MIPS_CP0_EXCEPTION_BASE, MIPS_CP0_EXCEPTION_BASE_SELECT
	ehb
	li $8, MIPS_STATUS_KERNEL_ADDRESSING
	mtc0 $8, MIPS_CP0_STATUS
	ehb

	/* Keeps the loader's four arguments in callee-saved registers. */
	move $16, $4
	move $17, $5
	move $18, $6
	move $19, $7

	/*
	 * Marks the first stage of the boot on the board.  The mark is a
	 * leaf that uses no stack and no global and keeps every register
	 * but its argument's, the temporaries and ra.
	 */
	jal mips_board_early_mark
	li $4, MIPS_EARLY_MARK_START

	/*
	 * Makes KSEG0 cacheable.  The cacheability of KSEG0 is changed from
	 * KSEG1, so that no instruction is fetched through KSEG0 while it
	 * changes; the return to KSEG0 clears the instruction hazard.
	 */
	LOAD_ADDRESS($8, 1f)
	li $9, MIPS_KSEG1_BASE - MIPS_KSEG0_BASE
	PTR_ADDU $8, $8, $9
	jr $8
	nop
1:
	mfc0 $8, MIPS_CP0_CONFIG, MIPS_CP0_CONFIG_SELECT
	li $9, ~MIPS_CONFIG_K0_MASK
	and $8, $8, $9
	ori $8, $8, MIPS_CONFIG_K0_CACHED
	mtc0 $8, MIPS_CP0_CONFIG, MIPS_CP0_CONFIG_SELECT
	ehb
	LOAD_ADDRESS($8, 2f)
	jr.hb $8
	nop
2:
	/* Clears the bss, a word at a time; the link script aligns both ends to eight bytes. */
	LOAD_ADDRESS($8, __bss_start)
	LOAD_ADDRESS($9, __bss_end)
3:
	beq $8, $9, 4f
	nop
	sw $0, 0($8)
	b 3b
	PTR_ADDIU $8, $8, 4
4:
	/* Enters C on the boot stack, below the home of its four arguments. */
	LOAD_ADDRESS($29, mips_boot_stack_top)
	PTR_ADDIU $29, $29, -MIPS_ARGUMENT_HOME_SIZE
	move $4, $16
	move $5, $17
	move $6, $18
	jal mips_cmain
	move $7, $19

	/* mips_cmain() does not return; waits here for good if it does. */
5:
	wait
	b 5b
	nop
	.size _start, . - _start

	/*
	 * The boot stack.  It is in the bss, which the code above clears
	 * before it moves onto it.
	 */
	.section .bss.boot, "aw", %nobits
	.balign 16
	.global mips_boot_stack
mips_boot_stack:
	.space MIPS_BOOT_STACK_SIZE
	.global mips_boot_stack_top
mips_boot_stack_top:
