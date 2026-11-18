/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC trap frame and the exception dispatch.
 */

#ifndef KERN_HAL_PPC32_INT_H
#define KERN_HAL_PPC32_INT_H

#include <hal/types.h>
#include "defs.h"

/*
 * What the entry saves of the interrupted context, on the kernel stack:
 * the general registers by their numbers, then the special registers.
 */
struct ppc32_trap_frame {
	uint32_t r[32];
	uint32_t lr;
	uint32_t ctr;
	uint32_t xer;
	uint32_t cr;
	uint32_t pc;
	uint32_t msr;
	uint32_t dar;
	uint32_t dsisr;
	uint32_t vector;
	uint32_t padding[3];
};

_Static_assert(sizeof(struct ppc32_trap_frame) == PPC32_FRAME_SIZE, "entry.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, lr) == PPC32_FRAME_LR, "entry.S saves lr here");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, cr) == PPC32_FRAME_CR, "entry.S saves cr here");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, pc) == PPC32_FRAME_PC, "entry.S saves srr0 here");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, msr) == PPC32_FRAME_MSR, "entry.S saves srr1 here");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, dsisr) == PPC32_FRAME_DSISR, "entry.S saves dsisr here");
_Static_assert(__builtin_offsetof(struct ppc32_trap_frame, vector) == PPC32_FRAME_VECTOR, "entry.S saves the vector here");

void ppc32_int_init(void);
void ppc32_trap(struct ppc32_trap_frame *frame);

#endif
