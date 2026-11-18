/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 trap frame and the exception dispatch.
 */

#ifndef KERN_HAL_M68K_M68040_INT_H
#define KERN_HAL_M68K_M68040_INT_H

#include <hal/types.h>
#include "defs.h"

/*
 * What the entry saves of the interrupted context, on the kernel stack:
 * the user stack pointer, the data and address registers (a7 is the stack
 * itself), then the first three fields of the processor's frame.  The
 * processor's frame may go on past them (its format says how far); the
 * fields after the format word are read through m68040_frame_word().
 */
struct m68040_trap_frame {
	uint32_t usp;
	uint32_t d[8];
	uint32_t a[7];
	uint16_t sr;
	uint32_t pc;
	uint16_t format;
} __attribute__((packed, aligned(2)));

_Static_assert(sizeof(struct m68040_trap_frame) == M68040_FRAME_SIZE,
	       "entry.S builds frames of this size");
_Static_assert(__builtin_offsetof(struct m68040_trap_frame, sr) == M68040_FRAME_SR,
	       "the processor's frame starts at this offset");
_Static_assert(__builtin_offsetof(struct m68040_trap_frame, format) == M68040_FRAME_FORMAT,
	       "the format word lies at this offset");

void m68040_int_init(void);
void m68040_trap(struct m68040_trap_frame *frame);

#endif
