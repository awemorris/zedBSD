/* Assembly/C contract for the software prefix ahead of an m68k HW frame. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_FRAME_OFFSETS_H
#define ZEDBSD_HAL_M68K_FRAME_OFFSETS_H

#define M68K_FRAME_USP_OFFSET       0
#define M68K_FRAME_D0_OFFSET        4
#define M68K_FRAME_A0_OFFSET        36
#define M68K_FRAME_HARDWARE_OFFSET  64
#define M68K_FRAME_SR_OFFSET        64
#define M68K_FRAME_PC_OFFSET        66
#define M68K_FRAME_FORMAT_OFFSET    70

#ifndef __ASSEMBLER__
#include <hal/types.h>

struct m68k_saved_frame {
	uint32_t usp;
	uint32_t d[8];
	uint32_t a[7];
	uint8_t hardware[];
};

_Static_assert(__builtin_offsetof(struct m68k_saved_frame, usp) ==
	M68K_FRAME_USP_OFFSET, "m68k frame USP offset");
_Static_assert(__builtin_offsetof(struct m68k_saved_frame, d[0]) ==
	M68K_FRAME_D0_OFFSET, "m68k frame D0 offset");
_Static_assert(__builtin_offsetof(struct m68k_saved_frame, a[0]) ==
	M68K_FRAME_A0_OFFSET, "m68k frame A0 offset");
_Static_assert(sizeof(struct m68k_saved_frame) ==
	M68K_FRAME_HARDWARE_OFFSET, "m68k software frame size");
#endif

#endif
