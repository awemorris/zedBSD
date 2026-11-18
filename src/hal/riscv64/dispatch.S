/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 task switch, the first entries of new tasks, and the
 * floating-point save and restore.
 *
 * The kernel's C code is built without floating point; the save and
 * restore below tell the assembler that they may use the F and D
 * extensions.
 *
 * The thread pointer is not switched: in the kernel it names the hart, and
 * a task resumed on another hart finds that hart's record in it.
 */

#include "defs.h"

	.option norelax

	.text
	.balign 4

	/*
	 * Switches stacks: keeps the callee-saved registers and the return
	 * address on the current stack, stores the stack pointer at *a0,
	 * loads the one at *a1 and pops the same words from it.
	 */
	.global asm_task_dispatch
	.type asm_task_dispatch, %function
asm_task_dispatch:
	/* Keeps the departing context's callee-saved registers and return address. */
	addi sp, sp, -112
	sd s0, 0(sp)
	sd s1, 8(sp)
	sd s2, 16(sp)
	sd s3, 24(sp)
	sd s4, 32(sp)
	sd s5, 40(sp)
	sd s6, 48(sp)
	sd s7, 56(sp)
	sd s8, 64(sp)
	sd s9, 72(sp)
	sd s10, 80(sp)
	sd s11, 88(sp)
	sd ra, 96(sp)

	/* Leaves the departing stack and takes the arriving one. */
	sd sp, 0(a0)
	ld sp, 0(a1)

	/* Pops the arriving context's registers and returns into it. */
	ld s0, 0(sp)
	ld s1, 8(sp)
	ld s2, 16(sp)
	ld s3, 24(sp)
	ld s4, 32(sp)
	ld s5, 40(sp)
	ld s6, 48(sp)
	ld s7, 56(sp)
	ld s8, 64(sp)
	ld s9, 72(sp)
	ld s10, 80(sp)
	ld s11, 88(sp)
	ld ra, 96(sp)
	addi sp, sp, 112
	ret
	.size asm_task_dispatch, . - asm_task_dispatch

	/*
	 * The first entry of a system task: s0 is its function and s1 the
	 * argument.  It first releases the task its hart switched away from,
	 * and starts with interrupts on, as a new task has no suspended
	 * scheduler call that would turn them back on.
	 */
	.global riscv64_kernel_task_entry
	.type riscv64_kernel_task_entry, %function
riscv64_kernel_task_entry:
	/* Releases the departed task, then turns interrupts on. */
	call riscv64_task_finish_switch
	csrsi sstatus, 2

	/* Runs the function with its argument; a system task never returns from it. */
	mv a0, s1
	jalr s0
	call riscv64_task_returned
1:	j 1b
	.size riscv64_kernel_task_entry, . - riscv64_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: the stack pointer is at the user frame it returns through.
	 */
	.global riscv64_user_frame_entry
	.type riscv64_user_frame_entry, %function
riscv64_user_frame_entry:
	/* Releases the departed task, marks the first frame used and returns to user. */
	call riscv64_task_finish_switch
	call riscv64_task_first_user_entry
	csrci sstatus, 2
	j riscv64_trap_return
	.size riscv64_user_frame_entry, . - riscv64_user_frame_entry

	.option push
	.option arch, +f, +d

	/* Saves f0 to f31 and fcsr at a0, the layout of struct hal_fpregs. */
	.global riscv64_fp_save
	.type riscv64_fp_save, %function
riscv64_fp_save:
	fsd f0, 0(a0)
	fsd f1, 8(a0)
	fsd f2, 16(a0)
	fsd f3, 24(a0)
	fsd f4, 32(a0)
	fsd f5, 40(a0)
	fsd f6, 48(a0)
	fsd f7, 56(a0)
	fsd f8, 64(a0)
	fsd f9, 72(a0)
	fsd f10, 80(a0)
	fsd f11, 88(a0)
	fsd f12, 96(a0)
	fsd f13, 104(a0)
	fsd f14, 112(a0)
	fsd f15, 120(a0)
	fsd f16, 128(a0)
	fsd f17, 136(a0)
	fsd f18, 144(a0)
	fsd f19, 152(a0)
	fsd f20, 160(a0)
	fsd f21, 168(a0)
	fsd f22, 176(a0)
	fsd f23, 184(a0)
	fsd f24, 192(a0)
	fsd f25, 200(a0)
	fsd f26, 208(a0)
	fsd f27, 216(a0)
	fsd f28, 224(a0)
	fsd f29, 232(a0)
	fsd f30, 240(a0)
	fsd f31, 248(a0)
	frcsr t0
	sw t0, 256(a0)
	ret
	.size riscv64_fp_save, . - riscv64_fp_save

	/* Restores f0 to f31 and fcsr from a0. */
	.global riscv64_fp_restore
	.type riscv64_fp_restore, %function
riscv64_fp_restore:
	fld f0, 0(a0)
	fld f1, 8(a0)
	fld f2, 16(a0)
	fld f3, 24(a0)
	fld f4, 32(a0)
	fld f5, 40(a0)
	fld f6, 48(a0)
	fld f7, 56(a0)
	fld f8, 64(a0)
	fld f9, 72(a0)
	fld f10, 80(a0)
	fld f11, 88(a0)
	fld f12, 96(a0)
	fld f13, 104(a0)
	fld f14, 112(a0)
	fld f15, 120(a0)
	fld f16, 128(a0)
	fld f17, 136(a0)
	fld f18, 144(a0)
	fld f19, 152(a0)
	fld f20, 160(a0)
	fld f21, 168(a0)
	fld f22, 176(a0)
	fld f23, 184(a0)
	fld f24, 192(a0)
	fld f25, 200(a0)
	fld f26, 208(a0)
	fld f27, 216(a0)
	fld f28, 224(a0)
	fld f29, 232(a0)
	fld f30, 240(a0)
	fld f31, 248(a0)
	lw t0, 256(a0)
	fscsr t0
	ret
	.size riscv64_fp_restore, . - riscv64_fp_restore

	.option pop
