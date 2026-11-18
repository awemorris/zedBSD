/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 exception entry and return, the task switch, the first
 * entries of new tasks, the floating-point save and restore, and the
 * control registers the C code cannot reach.
 *
 * Every vector enters at the same place.  The entry raises the interrupt
 * mask to 7, so a handler is never nested, saves the user stack pointer
 * and the registers below the processor's frame and calls the C
 * dispatcher with the frame; the return restores them and leaves through
 * rte, which unwinds the processor's frame by its format.
 */

#include "defs.h"

	.text
	.even

	.globl m68040_exception_entry
	.type m68040_exception_entry,@function
m68040_exception_entry:
	ori.w #0x0700,%sr
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	move.l %usp,%a0
	move.l %a0,-(%sp)
	move.l %sp,-(%sp)
	jsr m68040_trap
	addq.l #4,%sp

	/* Returns through the frame at sp; a new user task's first entry comes here too. */
	.globl m68040_trap_return
m68040_trap_return:
	move.l (%sp)+,%a0
	move.l %a0,%usp
	movem.l (%sp)+,%d0-%d7/%a0-%a6
	rte
	.size m68040_exception_entry,.-m68040_exception_entry

	/*
	 * Switches stacks: keeps d2 to d7 and a2 to a6 on the current stack,
	 * stores the stack pointer at the first argument, loads the one at
	 * the second and pops the same words from it.
	 */
	.globl m68040_task_dispatch
	.type m68040_task_dispatch,@function
m68040_task_dispatch:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
	movem.l %d2-%d7/%a2-%a6,-(%sp)
	move.l %sp,(%a0)
	move.l (%a1),%sp
	movem.l (%sp)+,%d2-%d7/%a2-%a6
	rts
	.size m68040_task_dispatch,.-m68040_task_dispatch

	/*
	 * The first entry of a system task: d2 is its function and d3 the
	 * argument.  It releases the task its processor switched away from
	 * and starts with interrupts on.
	 */
	.globl m68040_kernel_task_entry
	.type m68040_kernel_task_entry,@function
m68040_kernel_task_entry:
	jsr m68040_task_finish_switch
	andi.w #0xf8ff,%sr
	move.l %d3,-(%sp)
	move.l %d2,%a0
	jsr (%a0)
	addq.l #4,%sp
	jsr m68040_task_returned
1:	bra.s 1b
	.size m68040_kernel_task_entry,.-m68040_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: the stack pointer is at the frame it returns through.
	 */
	.globl m68040_user_frame_entry
	.type m68040_user_frame_entry,@function
m68040_user_frame_entry:
	jsr m68040_task_finish_switch
	jsr m68040_task_first_user_entry
	ori.w #0x0700,%sr
	bra m68040_trap_return
	.size m68040_user_frame_entry,.-m68040_user_frame_entry

	/* Waits for an interrupt with the mask open, then closes it again. */
	.globl m68040_cpu_idle
	.type m68040_cpu_idle,@function
m68040_cpu_idle:
	stop #0x2000
	ori.w #0x0700,%sr
	rts
	.size m68040_cpu_idle,.-m68040_cpu_idle

	/* Points the vector base at the table. */
	.globl m68040_set_vbr
	.type m68040_set_vbr,@function
m68040_set_vbr:
	move.l 4(%sp),%d0
	movec %d0,%vbr
	rts
	.size m68040_set_vbr,.-m68040_set_vbr

	/*
	 * Loads both root pointers with one root and drops every cached
	 * translation.
	 */
	.globl m68040_load_root
	.type m68040_load_root,@function
m68040_load_root:
	move.l 4(%sp),%d0
	pflusha
	movec %d0,%urp
	movec %d0,%srp
	pflusha
	rts
	.size m68040_load_root,.-m68040_load_root

	/* Drops every cached translation. */
	.globl m68040_flush_atc
	.type m68040_flush_atc,@function
m68040_flush_atc:
	pflusha
	rts
	.size m68040_flush_atc,.-m68040_flush_atc

	/*
	 * Turns translation on: the transparent windows, then the table walk.
	 * The arguments are the instruction and data windows for RAM, the
	 * data window for the device space and the root.
	 */
	.globl m68040_mmu_enable
	.type m68040_mmu_enable,@function
m68040_mmu_enable:
	move.l 4(%sp),%d0
	movec %d0,%itt0
	movec %d0,%dtt0
	moveq #0,%d0
	movec %d0,%itt1
	move.l 8(%sp),%d0
	movec %d0,%dtt1
	move.l 12(%sp),%d0
	pflusha
	movec %d0,%urp
	movec %d0,%srp
	move.l #M68040_TC_ENABLE,%d0
	movec %d0,%tc
	pflusha
	rts
	.size m68040_mmu_enable,.-m68040_mmu_enable

	/* Pushes every dirty data line to memory and empties both caches. */
	.globl m68040_cache_push_all
	.type m68040_cache_push_all,@function
m68040_cache_push_all:
	nop
	cpusha %bc
	rts
	.size m68040_cache_push_all,.-m68040_cache_push_all

	/* Turns the instruction and data caches on. */
	.globl m68040_cache_enable
	.type m68040_cache_enable,@function
m68040_cache_enable:
	cinva %bc
	move.l #0x80008000,%d0
	movec %d0,%cacr
	rts
	.size m68040_cache_enable,.-m68040_cache_enable

	/*
	 * Saves the floating-point unit's internal state and its registers to
	 * the record at the argument (struct hal_fpregs), and restores them.
	 * The internal state frame is not kept: the unit is idle between
	 * instructions at a task switch.
	 */
	.globl m68040_fp_save
	.type m68040_fp_save,@function
m68040_fp_save:
	move.l 4(%sp),%a0
	fmovem.x %fp0-%fp7,(%a0)
	fmove.l %fpcr,96(%a0)
	fmove.l %fpsr,100(%a0)
	fmove.l %fpiar,104(%a0)
	rts
	.size m68040_fp_save,.-m68040_fp_save

	.globl m68040_fp_restore
	.type m68040_fp_restore,@function
m68040_fp_restore:
	move.l 4(%sp),%a0
	fmovem.x (%a0),%fp0-%fp7
	fmove.l 96(%a0),%fpcr
	fmove.l 100(%a0),%fpsr
	fmove.l 104(%a0),%fpiar
	rts
	.size m68040_fp_restore,.-m68040_fp_restore

	/* The vector table: every vector enters at the same place. */
	.section .rodata.vectors,"a"
	.balign 1024
	.globl m68040_vector_table
m68040_vector_table:
	.rept 256
	.long m68040_exception_entry
	.endr
