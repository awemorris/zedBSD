/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 trap entry and return.
 *
 * Every trap enters here, in direct mode.  sscratch says where it came
 * from: while user code runs it holds the hart's per-CPU record, and while
 * the kernel runs it holds zero.  A trap from user swaps it with the
 * thread pointer, moves to the running task's kernel stack, and sets
 * sscratch to zero at once, so that a trap nested in the handling is seen
 * as one from the kernel.  The way back to user sets sscratch to the
 * record again with interrupts off, and after that only loads registers
 * from the frame, which is on the kernel stack and cannot fault.
 *
 * A trap from the kernel keeps the kernel's thread pointer and does not
 * restore it on return: a task switched away inside the handling may be
 * resumed on another hart, whose record the thread pointer then names.
 */

#include "defs.h"

	.option norelax

	.text
	.balign 4
	.global riscv32_trap_entry
	.type riscv32_trap_entry, %function
riscv32_trap_entry:
	csrrw tp, sscratch, tp
	bnez tp, riscv32_trap_from_user

	/* From the kernel: sscratch held zero; take the thread pointer back and keep sscratch zero. */
	csrrw tp, sscratch, tp
	addi sp, sp, -RISCV32_FRAME_SIZE
	sw t0, 20(sp)
	addi t0, sp, RISCV32_FRAME_SIZE
	sw t0, 8(sp)
	sw tp, 16(sp)
	j riscv32_trap_save

riscv32_trap_from_user:
	/* From user: the record is in tp; the user's stack waits in the record. */
	sw sp, RISCV32_PERCPU_USER_SP(tp)
	lw sp, RISCV32_PERCPU_KERNEL_SP(tp)
	addi sp, sp, -RISCV32_FRAME_SIZE
	sw t0, 20(sp)
	lw t0, RISCV32_PERCPU_USER_SP(tp)
	sw t0, 8(sp)
	csrr t0, sscratch
	sw t0, 16(sp)
	csrw sscratch, zero

riscv32_trap_save:
	/* The remaining registers; sp, tp and t0 are already in the frame. */
	sw ra, 4(sp)
	sw gp, 12(sp)
	sw t1, 24(sp)
	sw t2, 28(sp)
	sw s0, 32(sp)
	sw s1, 36(sp)
	sw a0, 40(sp)
	sw a1, 44(sp)
	sw a2, 48(sp)
	sw a3, 52(sp)
	sw a4, 56(sp)
	sw a5, 60(sp)
	sw a6, 64(sp)
	sw a7, 68(sp)
	sw s2, 72(sp)
	sw s3, 76(sp)
	sw s4, 80(sp)
	sw s5, 84(sp)
	sw s6, 88(sp)
	sw s7, 92(sp)
	sw s8, 96(sp)
	sw s9, 100(sp)
	sw s10, 104(sp)
	sw s11, 108(sp)
	sw t3, 112(sp)
	sw t4, 116(sp)
	sw t5, 120(sp)
	sw t6, 124(sp)

	/* The trap's supervisor registers. */
	csrr t0, sepc
	sw t0, RISCV32_FRAME_SEPC(sp)
	csrr t0, sstatus
	sw t0, RISCV32_FRAME_SSTATUS(sp)
	csrr t0, scause
	sw t0, RISCV32_FRAME_SCAUSE(sp)
	csrr t0, stval
	sw t0, RISCV32_FRAME_STVAL(sp)

	/* The kernel never reaches user memory through its user mapping. */
	li t0, RISCV32_SSTATUS_SUM
	csrc sstatus, t0

	mv a0, sp
	call riscv32_trap

	/*
	 * Returns through the frame at sp, with interrupts off from here.
	 * A new user task's first entry comes here too.
	 */
	.global riscv32_trap_return
riscv32_trap_return:
	csrci sstatus, 2
	lw t0, RISCV32_FRAME_SEPC(sp)
	csrw sepc, t0
	lw t0, RISCV32_FRAME_SSTATUS(sp)
	csrw sstatus, t0
	andi t0, t0, RISCV32_SSTATUS_SPP
	bnez t0, riscv32_trap_to_kernel

	/* To user: sscratch names the record again; tp and sp come last. */
	csrw sscratch, tp
	lw ra, 4(sp)
	lw gp, 12(sp)
	lw t0, 20(sp)
	lw t1, 24(sp)
	lw t2, 28(sp)
	lw s0, 32(sp)
	lw s1, 36(sp)
	lw a0, 40(sp)
	lw a1, 44(sp)
	lw a2, 48(sp)
	lw a3, 52(sp)
	lw a4, 56(sp)
	lw a5, 60(sp)
	lw a6, 64(sp)
	lw a7, 68(sp)
	lw s2, 72(sp)
	lw s3, 76(sp)
	lw s4, 80(sp)
	lw s5, 84(sp)
	lw s6, 88(sp)
	lw s7, 92(sp)
	lw s8, 96(sp)
	lw s9, 100(sp)
	lw s10, 104(sp)
	lw s11, 108(sp)
	lw t3, 112(sp)
	lw t4, 116(sp)
	lw t5, 120(sp)
	lw t6, 124(sp)
	lw tp, 16(sp)
	lw sp, 8(sp)
	sret

riscv32_trap_to_kernel:
	/* To the kernel: the thread pointer stays this hart's. */
	lw ra, 4(sp)
	lw gp, 12(sp)
	lw t0, 20(sp)
	lw t1, 24(sp)
	lw t2, 28(sp)
	lw s0, 32(sp)
	lw s1, 36(sp)
	lw a0, 40(sp)
	lw a1, 44(sp)
	lw a2, 48(sp)
	lw a3, 52(sp)
	lw a4, 56(sp)
	lw a5, 60(sp)
	lw a6, 64(sp)
	lw a7, 68(sp)
	lw s2, 72(sp)
	lw s3, 76(sp)
	lw s4, 80(sp)
	lw s5, 84(sp)
	lw s6, 88(sp)
	lw s7, 92(sp)
	lw s8, 96(sp)
	lw s9, 100(sp)
	lw s10, 104(sp)
	lw s11, 108(sp)
	lw t3, 112(sp)
	lw t4, 116(sp)
	lw t5, 120(sp)
	lw t6, 124(sp)
	lw sp, 8(sp)
	sret
	.size riscv32_trap_entry, . - riscv32_trap_entry
