/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC trap frame and the exception dispatch.
 */

#ifndef KERN_HAL_PPC64_INT_H
#define KERN_HAL_PPC64_INT_H

#include <hal/types.h>
#include "defs.h"

/*
 * What the entry saves of the interrupted context, on the kernel stack:
 * the general registers by their numbers, then the special registers.
 */
struct ppc64_trap_frame {
	uint64_t r[32];
	uint64_t lr;
	uint64_t ctr;
	uint64_t xer;
	uint64_t cr;
	uint64_t pc;
	uint64_t msr;
	uint64_t dar;
	uint64_t dsisr;
	uint64_t vector;
	uint64_t padding;
};

_Static_assert(sizeof(struct ppc64_trap_frame) == PPC64_FRAME_SIZE, "entry.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, lr) == PPC64_FRAME_LR, "entry.S saves lr here");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, cr) == PPC64_FRAME_CR, "entry.S saves cr here");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, pc) == PPC64_FRAME_PC, "entry.S saves srr0 here");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, msr) == PPC64_FRAME_MSR, "entry.S saves srr1 here");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, dsisr) == PPC64_FRAME_DSISR, "entry.S saves dsisr here");
_Static_assert(__builtin_offsetof(struct ppc64_trap_frame, vector) == PPC64_FRAME_VECTOR, "entry.S saves the vector here");

void
ppc64_int_init(
	void);

void
ppc64_trap(
	struct ppc64_trap_frame *frame);

#endif
