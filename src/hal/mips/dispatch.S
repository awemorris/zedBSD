/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS context switch and the first entry of a kernel task.
 *
 * A switch keeps only what the o32 and n64 calling conventions make the
 * callee save: s0 to s7, fp and ra, a whole register each, with sp
 * itself in the save slot.  (n64 also has gp callee-saved; the kernel
 * never uses gp, which the link of vmunix checks.)  Status is
 * neither saved nor loaded, so the interrupt mask and the interrupt
 * enable go on as they are; the caller's interrupt save and restore
 * bring IE back on each side.
 */

#include "asm.h"

	.set noreorder

	.text
	.balign 4

	/*
	 * Switches stacks: pushes the callee-saved registers, stores sp at
	 * a0, loads sp from a1 and pops the registers kept there.
	 */
	.global mips_task_dispatch
	.type mips_task_dispatch, %function
mips_task_dispatch:
	PTR_ADDIU $29, $29, -(MIPS_DISPATCH_WORDS * MIPS_REGISTER_BYTES)
	REG_S $16, 0 * MIPS_REGISTER_BYTES($29)
	REG_S $17, 1 * MIPS_REGISTER_BYTES($29)
	REG_S $18, 2 * MIPS_REGISTER_BYTES($29)
	REG_S $19, 3 * MIPS_REGISTER_BYTES($29)
	REG_S $20, 4 * MIPS_REGISTER_BYTES($29)
	REG_S $21, 5 * MIPS_REGISTER_BYTES($29)
	REG_S $22, 6 * MIPS_REGISTER_BYTES($29)
	REG_S $23, 7 * MIPS_REGISTER_BYTES($29)
	REG_S $30, MIPS_DISPATCH_FP * MIPS_REGISTER_BYTES($29)
	REG_S $31, MIPS_DISPATCH_RA * MIPS_REGISTER_BYTES($29)
	REG_S $29, 0($4)
	REG_L $29, 0($5)
	REG_L $16, 0 * MIPS_REGISTER_BYTES($29)
	REG_L $17, 1 * MIPS_REGISTER_BYTES($29)
	REG_L $18, 2 * MIPS_REGISTER_BYTES($29)
	REG_L $19, 3 * MIPS_REGISTER_BYTES($29)
	REG_L $20, 4 * MIPS_REGISTER_BYTES($29)
	REG_L $21, 5 * MIPS_REGISTER_BYTES($29)
	REG_L $22, 6 * MIPS_REGISTER_BYTES($29)
	REG_L $23, 7 * MIPS_REGISTER_BYTES($29)
	REG_L $30, MIPS_DISPATCH_FP * MIPS_REGISTER_BYTES($29)
	REG_L $31, MIPS_DISPATCH_RA * MIPS_REGISTER_BYTES($29)
	jr $31
	PTR_ADDIU $29, $29, MIPS_DISPATCH_WORDS * MIPS_REGISTER_BYTES
	.size mips_task_dispatch, . - mips_task_dispatch

	/*
	 * The first entry of a kernel task: s0 is its function and s1 its
	 * argument.  It first releases the task it was switched from, whose
	 * stack it no longer uses.  It starts the function with interrupts
	 * on, since a new task has no suspended switch that would turn them
	 * back on, and stops if the function returns.
	 */
	.global mips_kernel_task_entry
	.type mips_kernel_task_entry, %function
mips_kernel_task_entry:
	jal mips_task_finish_switch
	nop
	ei
	ehb
	move $4, $17
	jalr $16
	PTR_ADDIU $29, $29, -MIPS_ARGUMENT_HOME_SIZE
	jal mips_task_returned
	nop
1:
	wait
	b 1b
	nop
	.size mips_kernel_task_entry, . - mips_kernel_task_entry
