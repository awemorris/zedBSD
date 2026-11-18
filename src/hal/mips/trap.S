/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS general exception entry and return.
 *
 * Every exception but the TLB refill and the cache error comes here from
 * EBase + 0x180, interrupts too, since the processor has no vectored
 * interrupts.  k0 and k1 are the entry's alone.  An exception from user
 * mode moves to the kernel stack the HAL keeps for the running task; one
 * from the kernel stays on the stack it interrupted.  The frame keeps
 * every register the interrupted code may hold, and the handler runs in
 * kernel mode with Status.EXL clear and interrupts off.
 *
 * The way back writes the Status the handler left in the frame with EXL
 * set first, so that nothing interrupts the restore and the mode it
 * names takes effect only at eret.  The handler builds that Status: the
 * saved value is never trusted for a return to user.
 */

#include "asm.h"

	.set noreorder
	.set noat

	.text
	.balign 4
	.global mips_trap_entry
	.type mips_trap_entry, %function
mips_trap_entry:
	/* From user, the kernel stack; from the kernel, the stack that was interrupted. */
	mfc0 $26, MIPS_CP0_STATUS
	andi $26, $26, MIPS_STATUS_MODE_MASK
	beqz $26, 1f
	move $27, $29
	LOAD_ADDRESS($26, mips_kernel_stack_pointer)
	REG_L $29, 0($26)
1:
	PTR_ADDIU $29, $29, -MIPS_TRAP_STACK_BYTES

	/* The general registers; sp is the one that was interrupted, k0 and k1 are not kept. */
	REG_S $0, MIPS_ARGUMENT_HOME_SIZE + 0 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $1, MIPS_ARGUMENT_HOME_SIZE + 1 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $2, MIPS_ARGUMENT_HOME_SIZE + 2 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $3, MIPS_ARGUMENT_HOME_SIZE + 3 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $4, MIPS_ARGUMENT_HOME_SIZE + 4 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $5, MIPS_ARGUMENT_HOME_SIZE + 5 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $6, MIPS_ARGUMENT_HOME_SIZE + 6 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $7, MIPS_ARGUMENT_HOME_SIZE + 7 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + 8 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $9, MIPS_ARGUMENT_HOME_SIZE + 9 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $10, MIPS_ARGUMENT_HOME_SIZE + 10 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $11, MIPS_ARGUMENT_HOME_SIZE + 11 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $12, MIPS_ARGUMENT_HOME_SIZE + 12 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $13, MIPS_ARGUMENT_HOME_SIZE + 13 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $14, MIPS_ARGUMENT_HOME_SIZE + 14 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $15, MIPS_ARGUMENT_HOME_SIZE + 15 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $16, MIPS_ARGUMENT_HOME_SIZE + 16 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $17, MIPS_ARGUMENT_HOME_SIZE + 17 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $18, MIPS_ARGUMENT_HOME_SIZE + 18 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $19, MIPS_ARGUMENT_HOME_SIZE + 19 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $20, MIPS_ARGUMENT_HOME_SIZE + 20 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $21, MIPS_ARGUMENT_HOME_SIZE + 21 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $22, MIPS_ARGUMENT_HOME_SIZE + 22 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $23, MIPS_ARGUMENT_HOME_SIZE + 23 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $24, MIPS_ARGUMENT_HOME_SIZE + 24 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $25, MIPS_ARGUMENT_HOME_SIZE + 25 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $0, MIPS_ARGUMENT_HOME_SIZE + 26 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $0, MIPS_ARGUMENT_HOME_SIZE + 27 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $28, MIPS_ARGUMENT_HOME_SIZE + 28 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $27, MIPS_ARGUMENT_HOME_SIZE + 29 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $30, MIPS_ARGUMENT_HOME_SIZE + 30 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_S $31, MIPS_ARGUMENT_HOME_SIZE + 31 * MIPS_FRAME_REGISTER_BYTES($29)

	/* The multiply unit and the exception's coprocessor 0 registers. */
	mfhi $8
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_HI($29)
	mflo $8
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_LO($29)
	MFC0_WIDE $8, MIPS_CP0_EXCEPTION_PC
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_EXCEPTION_PC($29)
	mfc0 $8, MIPS_CP0_CAUSE
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_CAUSE($29)
	MFC0_WIDE $8, MIPS_CP0_BAD_ADDRESS
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_BAD_ADDRESS($29)
	mfc0 $8, MIPS_CP0_STATUS
	REG_S $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_STATUS($29)

	/* Runs the handler in kernel mode, out of the exception level, with interrupts off. */
	li $9, ~(MIPS_STATUS_MODE_MASK | MIPS_STATUS_EXCEPTION_LEVEL | MIPS_STATUS_INTERRUPTS)
	and $8, $8, $9
	mtc0 $8, MIPS_CP0_STATUS
	ehb
	jal mips_trap
	PTR_ADDIU $4, $29, MIPS_ARGUMENT_HOME_SIZE

	/*
	 * Returns through the frame at sp.  Status goes back first, with
	 * the exception level set, so that no interrupt is taken from here
	 * to eret and the frame's mode starts only there.
	 */
	.global mips_trap_return
mips_trap_return:
	di
	ehb
	REG_L $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_STATUS($29)
	ori $8, $8, MIPS_STATUS_EXCEPTION_LEVEL
	mtc0 $8, MIPS_CP0_STATUS
	ehb
	REG_L $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_EXCEPTION_PC($29)
	MTC0_WIDE $8, MIPS_CP0_EXCEPTION_PC
	REG_L $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_HI($29)
	mthi $8
	REG_L $8, MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_LO($29)
	mtlo $8

	/* The general registers but sp, which comes last; k0 and k1 are not restored. */
	REG_L $1, MIPS_ARGUMENT_HOME_SIZE + 1 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $2, MIPS_ARGUMENT_HOME_SIZE + 2 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $3, MIPS_ARGUMENT_HOME_SIZE + 3 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $4, MIPS_ARGUMENT_HOME_SIZE + 4 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $5, MIPS_ARGUMENT_HOME_SIZE + 5 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $6, MIPS_ARGUMENT_HOME_SIZE + 6 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $7, MIPS_ARGUMENT_HOME_SIZE + 7 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $8, MIPS_ARGUMENT_HOME_SIZE + 8 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $9, MIPS_ARGUMENT_HOME_SIZE + 9 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $10, MIPS_ARGUMENT_HOME_SIZE + 10 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $11, MIPS_ARGUMENT_HOME_SIZE + 11 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $12, MIPS_ARGUMENT_HOME_SIZE + 12 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $13, MIPS_ARGUMENT_HOME_SIZE + 13 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $14, MIPS_ARGUMENT_HOME_SIZE + 14 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $15, MIPS_ARGUMENT_HOME_SIZE + 15 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $16, MIPS_ARGUMENT_HOME_SIZE + 16 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $17, MIPS_ARGUMENT_HOME_SIZE + 17 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $18, MIPS_ARGUMENT_HOME_SIZE + 18 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $19, MIPS_ARGUMENT_HOME_SIZE + 19 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $20, MIPS_ARGUMENT_HOME_SIZE + 20 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $21, MIPS_ARGUMENT_HOME_SIZE + 21 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $22, MIPS_ARGUMENT_HOME_SIZE + 22 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $23, MIPS_ARGUMENT_HOME_SIZE + 23 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $24, MIPS_ARGUMENT_HOME_SIZE + 24 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $25, MIPS_ARGUMENT_HOME_SIZE + 25 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $28, MIPS_ARGUMENT_HOME_SIZE + 28 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $30, MIPS_ARGUMENT_HOME_SIZE + 30 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $31, MIPS_ARGUMENT_HOME_SIZE + 31 * MIPS_FRAME_REGISTER_BYTES($29)
	REG_L $29, MIPS_ARGUMENT_HOME_SIZE + 29 * MIPS_FRAME_REGISTER_BYTES($29)
	eret
	.size mips_trap_entry, . - mips_trap_entry

	/*
	 * The first entry of a user task.  The task's first switch pops into
	 * here with sp at the argument home below the frame it was built
	 * with; the C side finishes the switch and builds the frame's Status,
	 * and the return path enters user mode through that frame.
	 */
	.global mips_user_frame_entry
	.type mips_user_frame_entry, %function
mips_user_frame_entry:
	jal mips_task_first_user_entry
	nop
	j mips_trap_return
	nop
	.size mips_user_frame_entry, . - mips_user_frame_entry
