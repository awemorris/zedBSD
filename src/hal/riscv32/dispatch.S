/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 task switch, the first entries of new tasks, and the
 * floating-point save and restore.
 *
 * The kernel's C code is built without floating point; the save and
 * restore below tell the assembler that they may use the F and D
 * extensions.
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
	addi sp, sp, -64
	sw s0, 0(sp)
	sw s1, 4(sp)
	sw s2, 8(sp)
	sw s3, 12(sp)
	sw s4, 16(sp)
	sw s5, 20(sp)
	sw s6, 24(sp)
	sw s7, 28(sp)
	sw s8, 32(sp)
	sw s9, 36(sp)
	sw s10, 40(sp)
	sw s11, 44(sp)
	sw ra, 48(sp)
	sw sp, 0(a0)
	lw sp, 0(a1)
	lw s0, 0(sp)
	lw s1, 4(sp)
	lw s2, 8(sp)
	lw s3, 12(sp)
	lw s4, 16(sp)
	lw s5, 20(sp)
	lw s6, 24(sp)
	lw s7, 28(sp)
	lw s8, 32(sp)
	lw s9, 36(sp)
	lw s10, 40(sp)
	lw s11, 44(sp)
	lw ra, 48(sp)
	addi sp, sp, 64
	ret
	.size asm_task_dispatch, . - asm_task_dispatch

	/*
	 * The first entry of a system task: s0 is its function and s1 the
	 * argument.  It first releases the task its hart switched away from,
	 * and starts with interrupts on, as a new task has no suspended
	 * scheduler call that would turn them back on.
	 */
	.global riscv32_kernel_task_entry
	.type riscv32_kernel_task_entry, %function
riscv32_kernel_task_entry:
	call riscv32_task_finish_switch
	csrsi sstatus, 2
	mv a0, s1
	jalr s0
	call riscv32_task_returned
1:	j 1b
	.size riscv32_kernel_task_entry, . - riscv32_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: the stack pointer is at the user frame it returns through.
	 */
	.global riscv32_user_frame_entry
	.type riscv32_user_frame_entry, %function
riscv32_user_frame_entry:
	call riscv32_task_finish_switch
	call riscv32_task_first_user_entry
	csrci sstatus, 2
	j riscv32_trap_return
	.size riscv32_user_frame_entry, . - riscv32_user_frame_entry

	.option push
	.option arch, +f, +d

	/* Saves f0 to f31 and fcsr at a0, the layout of struct hal_fpregs. */
	.global riscv32_fp_save
	.type riscv32_fp_save, %function
riscv32_fp_save:
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
	.size riscv32_fp_save, . - riscv32_fp_save

	/* Restores f0 to f31 and fcsr from a0. */
	.global riscv32_fp_restore
	.type riscv32_fp_restore, %function
riscv32_fp_restore:
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
	.size riscv32_fp_restore, . - riscv32_fp_restore

	.option pop
