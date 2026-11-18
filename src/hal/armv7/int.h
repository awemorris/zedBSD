/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 trap frame and the trap entry's C side.
 */

#ifndef KERN_HAL_ARMV7_INT_H
#define KERN_HAL_ARMV7_INT_H

#include <hal/types.h>
#include "defs.h"

/* The register slots of the frame that hold the system call's number and result. */
#define ARMV7_FRAME_R0		0U
#define ARMV7_FRAME_R7		7U

/*
 * What the trap entry saves of the interrupted context, on the supervisor
 * stack: r0 to r12, the user's banked stack pointer and link register,
 * the interrupted supervisor link register and stack pointer, a scratch
 * word the return writes to close the exclusive monitor, and the return
 * address and status srsdb stored.  A frame lives from the trap's entry to
 * its return; a task's user frame is the one the task entered the kernel
 * with.
 */
struct armv7_trap_frame {
	uint32_t r[13];
	uint32_t sp_usr;
	uint32_t lr_usr;
	uint32_t lr_svc;
	uint32_t sp_svc;
	uint32_t scratch;
	uint32_t pc;
	uint32_t cpsr;
};

void
armv7_trap(
	struct armv7_trap_frame *frame,
	uint32_t kind);

int
armv7_trap_pending_abort(
	uint32_t *status,
	uint32_t *address);

void
armv7_trap_return_refused(
	struct armv7_trap_frame *frame);

void
armv7_trap_return(
	void);

_Static_assert(sizeof(struct armv7_trap_frame) == ARMV7_FRAME_SIZE,
	       "vectors.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, sp_usr) == ARMV7_FRAME_SP_USR,
	       "vectors.S saves the user stack pointer at this offset");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, lr_usr) == ARMV7_FRAME_LR_USR,
	       "vectors.S saves the user link register at this offset");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, lr_svc) == ARMV7_FRAME_LR_SVC,
	       "vectors.S saves the supervisor link register at this offset");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, sp_svc) == ARMV7_FRAME_SP_SVC,
	       "vectors.S saves the supervisor stack pointer at this offset");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, scratch) == ARMV7_FRAME_SCRATCH,
	       "vectors.S closes the monitor on this word");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, pc) == ARMV7_FRAME_PC,
	       "srsdb stores the return address at this offset");
_Static_assert(__builtin_offsetof(struct armv7_trap_frame, cpsr) == ARMV7_FRAME_CPSR,
	       "srsdb stores the status at this offset");

#endif
