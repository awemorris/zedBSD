/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS trap frame and the trap handling's entry points.
 */

#ifndef KERN_HAL_MIPS_TRAP_H
#define KERN_HAL_MIPS_TRAP_H

#include <hal/types.h>
#include "asm.h"

/*
 * The general register numbers the trap handling reads or writes in a
 * frame: v0 and a3, which carry a system call's result and its error
 * mark, the argument registers a0 to a3 and t0, t1, which carry its
 * arguments, t9, which holds a called function's own address in
 * position-independent code, the stack pointer and the return address.
 */
#define MIPS_REGISTER_V0		2
#define MIPS_REGISTER_A0		4
#define MIPS_REGISTER_A1		5
#define MIPS_REGISTER_A2		6
#define MIPS_REGISTER_A3		7
#define MIPS_REGISTER_T0		8
#define MIPS_REGISTER_T1		9
#define MIPS_REGISTER_T9		25
#define MIPS_REGISTER_SP		29
#define MIPS_REGISTER_RA		31

/*
 * The state of the code an exception interrupted, as trap.S saves it.
 *
 * One frame lives on the stack the exception moved to, from the entry to
 * the eret; the handler may change it, and the return loads what it
 * holds, except that Status is replaced before every return.  Each
 * member is a whole register (Status and Cause sign-extended on the
 * 64-bit processor).
 */
struct mips_trap_frame {
	uintptr_t r[32];
	uintptr_t hi;
	uintptr_t lo;
	uintptr_t exception_pc;
	uintptr_t status;
	uintptr_t cause;
	uintptr_t bad_address;
};

_Static_assert(__builtin_offsetof(struct mips_trap_frame, hi) == MIPS_FRAME_HI, "trap frame hi");
_Static_assert(__builtin_offsetof(struct mips_trap_frame, lo) == MIPS_FRAME_LO, "trap frame lo");
_Static_assert(__builtin_offsetof(struct mips_trap_frame, exception_pc) == MIPS_FRAME_EXCEPTION_PC, "trap frame epc");
_Static_assert(__builtin_offsetof(struct mips_trap_frame, status) == MIPS_FRAME_STATUS, "trap frame status");
_Static_assert(__builtin_offsetof(struct mips_trap_frame, cause) == MIPS_FRAME_CAUSE, "trap frame cause");
_Static_assert(__builtin_offsetof(struct mips_trap_frame, bad_address) == MIPS_FRAME_BAD_ADDRESS, "trap frame badvaddr");
_Static_assert(sizeof(struct mips_trap_frame) == MIPS_FRAME_SIZE, "trap frame size");
_Static_assert((MIPS_TRAP_STACK_BYTES % (2 * MIPS_REGISTER_BYTES)) == 0, "trap frame alignment");

/*
 * The top of the kernel stack an exception from user mode moves to.  The
 * processor runs one task at a time, so one word serves; the task switch
 * sets it.  Zero until a task that enters user mode runs.
 */
extern uintptr_t mips_kernel_stack_pointer;

void
mips_trap(
	struct mips_trap_frame *frame);

void
mips_trap_selftest(
	void);

/*
 * What the trap handling passes for a fault that is not a TLB exception,
 * where the selected space's finding (MIPS_SPACE_FAULT_*) would be.
 */
#define MIPS_TRAP_NOT_SPACE	(-1)

/*
 * A handler the paging self-test installs for the kernel's own faults
 * while it runs, given the frame and what the space found: it reports
 * nonzero when it took the fault, which then goes past the faulting
 * instruction.  NULL outside the test.
 */
typedef int (*mips_trap_fault_hook_t)(const struct mips_trap_frame *frame, int fault);

void
mips_trap_set_fault_hook(
	mips_trap_fault_hook_t hook);

#endif
