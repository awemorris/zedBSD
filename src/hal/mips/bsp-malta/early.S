/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's marks of the boot before the console shows.
 *
 * locore.S calls mips_board_early_mark() before the bss and the stack
 * exist, so it is a leaf that uses neither.  The emulator's screen shows
 * the console from the start, so the malta board marks nothing.
 */

#include "../asm.h"

	.set noreorder

	.text
	.balign 4
	.global mips_board_early_mark
	.type mips_board_early_mark, %function
mips_board_early_mark:
	jr $31
	nop
	.size mips_board_early_mark, . - mips_board_early_mark
