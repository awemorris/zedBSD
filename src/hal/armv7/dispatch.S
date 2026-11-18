/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 task switch and the first entries of new tasks.
 *
 * The kernel runs in supervisor mode only, so a task's kernel context is
 * its supervisor stack: the switch keeps the callee-saved registers on the
 * stack it leaves and takes them from the one it enters.  The per-CPU
 * record lives in TPIDRPRW and is not switched.
 */

#include "defs.h"

	.syntax unified
	.arm
	.text

	/*
	 * Switches stacks: keeps r4 to r11 and the link register on the
	 * current stack, with one word that keeps it 8-byte aligned, stores
	 * the stack pointer at *r0, loads the one at *r1 and pops the same
	 * words from it.
	 */
	.balign 4
	.global asm_task_dispatch
	.type asm_task_dispatch, %function
asm_task_dispatch:
	push {r4-r11, lr}
	sub sp, sp, #4
	str sp, [r0]
	ldr sp, [r1]
	add sp, sp, #4
	pop {r4-r11, lr}
	bx lr
	.size asm_task_dispatch, . - asm_task_dispatch

	/*
	 * The first entry of a system task: r4 is its function and r5 the
	 * argument.  It first releases the task its processor switched away
	 * from, and starts with interrupts on, as a new task has no suspended
	 * scheduler call that would turn them back on.
	 */
	.balign 4
	.global armv7_kernel_task_entry
	.type armv7_kernel_task_entry, %function
armv7_kernel_task_entry:
	bl armv7_task_finish_switch
	cpsie i
	mov r0, r5
	blx r4
	bl armv7_task_returned
1:	b 1b
	.size armv7_kernel_task_entry, . - armv7_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: the stack pointer is at the user frame it returns through,
	 * at the top of its kernel stack.
	 */
	.balign 4
	.global armv7_user_frame_entry
	.type armv7_user_frame_entry, %function
armv7_user_frame_entry:
	bl armv7_task_finish_switch
	bl armv7_task_first_user_entry
	cpsid i
	b armv7_trap_return
	.size armv7_user_frame_entry, . - armv7_user_frame_entry
