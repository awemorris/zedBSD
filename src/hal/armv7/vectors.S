/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 exception vectors, the trap entry and the return.
 *
 * Every exception is handled in supervisor mode on the supervisor stack,
 * whatever mode it was taken in: srsdb stores the return address and the
 * saved status on that stack, cps moves to supervisor mode, and the rest
 * of the frame is built below them.  While user code runs, the supervisor
 * stack pointer is the top of the running task's kernel stack, so a trap
 * from user builds its frame there; a trap from the kernel builds it on
 * the interrupted code's own stack.  The other modes' stacks are never
 * used.
 *
 * The frame is 80 bytes, but the supervisor stack may be only 4-byte
 * aligned where the kernel was interrupted, so the C handler runs on a
 * stack aligned down to 8 bytes below the frame and is passed the frame's
 * address.
 *
 * Every return closes the exclusive monitor with a store-exclusive to the
 * frame's scratch word, so that a load-exclusive of the interrupted code
 * cannot be paired with a store-exclusive after the handler ran (Linux
 * v6.12 arch/arm/kernel/entry-header.S does the same rather than clrex,
 * for Cortex-A15 erratum 830321).
 */

#include "defs.h"

	.syntax unified
	.arm

	/*
	 * Builds the frame of an exception taken in the current mode and
	 * enters the common path with the exception's kind.  adjust is how
	 * far the mode's link register is past the return address.
	 */
	.macro TRAP_ENTRY adjust, kind
	.if \adjust
	sub lr, lr, #\adjust
	.endif
	srsdb sp!, #ARMV7_PSR_MODE_SVC
	cps #ARMV7_PSR_MODE_SVC
	sub sp, sp, #ARMV7_FRAME_PC
	stmia sp, {r0-r12}
	add r0, sp, #ARMV7_FRAME_SP_USR
	stmia r0, {sp, lr}^
	nop
	str lr, [sp, #ARMV7_FRAME_LR_SVC]
	add r0, sp, #ARMV7_FRAME_SIZE
	str r0, [sp, #ARMV7_FRAME_SP_SVC]
	mov r1, #\kind
	b armv7_trap_common
	.endm

	.text

	/* The table VBAR points at: one branch per exception. */
	.balign 32
	.global armv7_vectors
armv7_vectors:
	b armv7_vector_reset
	b armv7_vector_undefined
	b armv7_vector_svc
	b armv7_vector_prefetch_abort
	b armv7_vector_data_abort
	b armv7_vector_unused
	b armv7_vector_irq
	b armv7_vector_fiq

	/* The undefined instruction's address is 4 below the link register. */
armv7_vector_undefined:
	TRAP_ENTRY 4, ARMV7_TRAP_UNDEFINED

	/* A system call returns to the instruction after it, where the link register is. */
armv7_vector_svc:
	TRAP_ENTRY 0, ARMV7_TRAP_SVC

	/* A prefetch abort restarts the instruction 4 below the link register. */
armv7_vector_prefetch_abort:
	TRAP_ENTRY 4, ARMV7_TRAP_PREFETCH_ABORT

	/* A data abort restarts the instruction 8 below the link register. */
armv7_vector_data_abort:
	TRAP_ENTRY 8, ARMV7_TRAP_DATA_ABORT

	/* An interrupt returns to the instruction 4 below the link register. */
armv7_vector_irq:
	TRAP_ENTRY 4, ARMV7_TRAP_IRQ

	/* The vectors the HAL never expects: reported with the frame they came with. */
armv7_vector_reset:
	TRAP_ENTRY 0, ARMV7_TRAP_RESET
armv7_vector_unused:
	TRAP_ENTRY 0, ARMV7_TRAP_UNUSED
armv7_vector_fiq:
	TRAP_ENTRY 4, ARMV7_TRAP_FIQ

	/*
	 * Calls the C handler with the frame (r0) and the kind (r1) on a
	 * stack aligned to 8 bytes, then returns through the frame.
	 */
	.balign 4
armv7_trap_common:
	mov r0, sp
	mov r4, sp
	bic sp, sp, #7
	bl armv7_trap
	mov sp, r4

	/*
	 * Returns through the frame at sp with interrupts masked.  A new
	 * task's first return to user comes here too.
	 */
	.global armv7_trap_return
	.type armv7_trap_return, %function
armv7_trap_return:
	cpsid i
	ldr r1, [sp, #ARMV7_FRAME_CPSR]
	and r2, r1, #ARMV7_PSR_MODE_MASK
	cmp r2, #ARMV7_PSR_MODE_USR
	bne 2f

	/* To user: only with interrupts unmasked there, so a frame cannot leave user code deaf. */
	tst r1, #(ARMV7_PSR_I | ARMV7_PSR_F)
	bne armv7_trap_bad_return
	add r0, sp, #ARMV7_FRAME_SCRATCH
	strex r2, r1, [r0]
	add r0, sp, #ARMV7_FRAME_SP_USR
	ldmia r0, {sp, lr}^
	nop
	ldmia sp, {r0-r12}
	add sp, sp, #ARMV7_FRAME_PC
	rfeia sp!

	/* To the kernel: its link register comes back, and its stack pointer is past the frame. */
2:	add r0, sp, #ARMV7_FRAME_SCRATCH
	strex r2, r1, [r0]
	ldr lr, [sp, #ARMV7_FRAME_LR_SVC]
	ldmia sp, {r0-r12}
	add sp, sp, #ARMV7_FRAME_PC
	rfeia sp!

	/* A frame that would enter user mode with interrupts masked stops the kernel. */
armv7_trap_bad_return:
	mov r0, sp
	bic sp, sp, #7
	bl armv7_trap_return_refused
	b armv7_halt_loop
	.size armv7_trap_return, . - armv7_trap_return
