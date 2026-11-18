/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 trap entry and return.
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
	.global riscv64_trap_entry
	.type riscv64_trap_entry, %function
riscv64_trap_entry:
	/* Swaps tp with sscratch; a nonzero value is the record of a trap from user. */
	csrrw tp, sscratch, tp
	bnez tp, riscv64_trap_from_user

	/* From the kernel: sscratch held zero; take the thread pointer back and keep sscratch zero. */
	csrrw tp, sscratch, tp
	addi sp, sp, -RISCV64_FRAME_SIZE
	sd t0, 40(sp)
	addi t0, sp, RISCV64_FRAME_SIZE
	sd t0, 16(sp)
	sd tp, 32(sp)
	j riscv64_trap_save

riscv64_trap_from_user:
	/* From user: the record is in tp; the user's stack waits in the record. */
	sd sp, RISCV64_PERCPU_USER_SP(tp)
	ld sp, RISCV64_PERCPU_KERNEL_SP(tp)
	addi sp, sp, -RISCV64_FRAME_SIZE
	sd t0, 40(sp)
	ld t0, RISCV64_PERCPU_USER_SP(tp)
	sd t0, 16(sp)
	csrr t0, sscratch
	sd t0, 32(sp)
	csrw sscratch, zero

riscv64_trap_save:
	/* The remaining registers; sp, tp and t0 are already in the frame. */
	sd ra, 8(sp)
	sd gp, 24(sp)
	sd t1, 48(sp)
	sd t2, 56(sp)
	sd s0, 64(sp)
	sd s1, 72(sp)
	sd a0, 80(sp)
	sd a1, 88(sp)
	sd a2, 96(sp)
	sd a3, 104(sp)
	sd a4, 112(sp)
	sd a5, 120(sp)
	sd a6, 128(sp)
	sd a7, 136(sp)
	sd s2, 144(sp)
	sd s3, 152(sp)
	sd s4, 160(sp)
	sd s5, 168(sp)
	sd s6, 176(sp)
	sd s7, 184(sp)
	sd s8, 192(sp)
	sd s9, 200(sp)
	sd s10, 208(sp)
	sd s11, 216(sp)
	sd t3, 224(sp)
	sd t4, 232(sp)
	sd t5, 240(sp)
	sd t6, 248(sp)

	/* The trap's supervisor registers. */
	csrr t0, sepc
	sd t0, RISCV64_FRAME_SEPC(sp)
	csrr t0, sstatus
	sd t0, RISCV64_FRAME_SSTATUS(sp)
	csrr t0, scause
	sd t0, RISCV64_FRAME_SCAUSE(sp)
	csrr t0, stval
	sd t0, RISCV64_FRAME_STVAL(sp)

	/* The kernel never reaches user memory through its user mapping. */
	li t0, RISCV64_SSTATUS_SUM
	csrc sstatus, t0

	/* Hands the frame to C. */
	mv a0, sp
	call riscv64_trap

	/*
	 * Returns through the frame at sp, with interrupts off from here.
	 * A new user task's first entry comes here too.
	 */
	.global riscv64_trap_return
riscv64_trap_return:
	/* Restores the return address and the state; the previous mode picks the way back. */
	csrci sstatus, 2
	ld t0, RISCV64_FRAME_SEPC(sp)
	csrw sepc, t0
	ld t0, RISCV64_FRAME_SSTATUS(sp)
	csrw sstatus, t0
	andi t0, t0, RISCV64_SSTATUS_SPP
	bnez t0, riscv64_trap_to_kernel

	/* To user: sscratch names the record again; tp and sp come last. */
	csrw sscratch, tp
	ld ra, 8(sp)
	ld gp, 24(sp)
	ld t0, 40(sp)
	ld t1, 48(sp)
	ld t2, 56(sp)
	ld s0, 64(sp)
	ld s1, 72(sp)
	ld a0, 80(sp)
	ld a1, 88(sp)
	ld a2, 96(sp)
	ld a3, 104(sp)
	ld a4, 112(sp)
	ld a5, 120(sp)
	ld a6, 128(sp)
	ld a7, 136(sp)
	ld s2, 144(sp)
	ld s3, 152(sp)
	ld s4, 160(sp)
	ld s5, 168(sp)
	ld s6, 176(sp)
	ld s7, 184(sp)
	ld s8, 192(sp)
	ld s9, 200(sp)
	ld s10, 208(sp)
	ld s11, 216(sp)
	ld t3, 224(sp)
	ld t4, 232(sp)
	ld t5, 240(sp)
	ld t6, 248(sp)
	ld tp, 32(sp)
	ld sp, 16(sp)
	sret

riscv64_trap_to_kernel:
	/* To the kernel: the thread pointer stays this hart's. */
	ld ra, 8(sp)
	ld gp, 24(sp)
	ld t0, 40(sp)
	ld t1, 48(sp)
	ld t2, 56(sp)
	ld s0, 64(sp)
	ld s1, 72(sp)
	ld a0, 80(sp)
	ld a1, 88(sp)
	ld a2, 96(sp)
	ld a3, 104(sp)
	ld a4, 112(sp)
	ld a5, 120(sp)
	ld a6, 128(sp)
	ld a7, 136(sp)
	ld s2, 144(sp)
	ld s3, 152(sp)
	ld s4, 160(sp)
	ld s5, 168(sp)
	ld s6, 176(sp)
	ld s7, 184(sp)
	ld s8, 192(sp)
	ld s9, 200(sp)
	ld s10, 208(sp)
	ld s11, 216(sp)
	ld t3, 224(sp)
	ld t4, 232(sp)
	ld t5, 240(sp)
	ld t6, 248(sp)
	ld sp, 16(sp)
	sret
	.size riscv64_trap_entry, . - riscv64_trap_entry
