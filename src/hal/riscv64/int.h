/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 trap frame and the trap entry's C side.
 */

#ifndef KERN_HAL_RISCV64_INT_H
#define KERN_HAL_RISCV64_INT_H

#include <hal/types.h>
#include "defs.h"

/*
 * The register slots of the frame the HAL reads by name: the return
 * address, the stack and thread pointers, the first argument (and result)
 * and the system call number.
 */
#define RISCV64_FRAME_SP	2U
#define RISCV64_FRAME_TP	4U
#define RISCV64_FRAME_A0	10U
#define RISCV64_FRAME_A7	17U
#define RISCV64_FRAME_RA	1U

/*
 * What the trap entry saves of the interrupted context, on the kernel
 * stack: the registers by their numbers (slot 0 is unused, as x0 always
 * reads zero), then the trap's supervisor registers.  A frame lives from
 * the trap's entry to its return; a task's user frame is the one the task
 * entered the kernel with.
 */
struct riscv64_trap_frame {
	uint64_t x[32];
	uint64_t sepc;
	uint64_t sstatus;
	uint64_t scause;
	uint64_t stval;
};

_Static_assert(sizeof(struct riscv64_trap_frame) == RISCV64_FRAME_SIZE,
	       "trap.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct riscv64_trap_frame, sepc) == RISCV64_FRAME_SEPC,
	       "trap.S saves sepc at this offset");
_Static_assert(__builtin_offsetof(struct riscv64_trap_frame, sstatus) == RISCV64_FRAME_SSTATUS,
	       "trap.S saves sstatus at this offset");
_Static_assert(__builtin_offsetof(struct riscv64_trap_frame, scause) == RISCV64_FRAME_SCAUSE,
	       "trap.S saves scause at this offset");
_Static_assert(__builtin_offsetof(struct riscv64_trap_frame, stval) == RISCV64_FRAME_STVAL,
	       "trap.S saves stval at this offset");

void
riscv64_int_init(
	void);

void
riscv64_int_init_secondary(
	void);

void
riscv64_trap(
	struct riscv64_trap_frame *frame);

#endif
