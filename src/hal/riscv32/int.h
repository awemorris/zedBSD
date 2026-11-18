/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 trap frame and the trap entry's C side.
 */

#ifndef KERN_HAL_RISCV32_INT_H
#define KERN_HAL_RISCV32_INT_H

#include <hal/types.h>
#include "defs.h"

/* The register slots of the frame that hold the stack and thread pointers. */
#define RISCV32_FRAME_SP	2U
#define RISCV32_FRAME_TP	4U
#define RISCV32_FRAME_A0	10U
#define RISCV32_FRAME_A7	17U
#define RISCV32_FRAME_RA	1U

/*
 * What the trap entry saves of the interrupted context, on the kernel
 * stack: the registers by their numbers (slot 0 is unused, as x0 always
 * reads zero), then the trap's supervisor registers.  A frame lives from
 * the trap's entry to its return; a task's user frame is the one the task
 * entered the kernel with.
 */
struct riscv32_trap_frame {
	unsigned long x[32];
	unsigned long sepc;
	unsigned long sstatus;
	unsigned long scause;
	unsigned long stval;
};

void
riscv32_int_init(
	void);

void
riscv32_int_init_secondary(
	void);

void
riscv32_trap(
	struct riscv32_trap_frame *frame);

_Static_assert(sizeof(struct riscv32_trap_frame) == RISCV32_FRAME_SIZE,
	       "trap.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct riscv32_trap_frame, sepc) == RISCV32_FRAME_SEPC,
	       "trap.S saves sepc at this offset");
_Static_assert(__builtin_offsetof(struct riscv32_trap_frame, sstatus) == RISCV32_FRAME_SSTATUS,
	       "trap.S saves sstatus at this offset");
_Static_assert(__builtin_offsetof(struct riscv32_trap_frame, scause) == RISCV32_FRAME_SCAUSE,
	       "trap.S saves scause at this offset");
_Static_assert(__builtin_offsetof(struct riscv32_trap_frame, stval) == RISCV32_FRAME_STVAL,
	       "trap.S saves stval at this offset");

#endif
