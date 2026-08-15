/* m68k soft-float user ABI link contract fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.globl _start
	.type _start,@function
_start:
	moveq #0,%d0
	rts
	.size _start,.-_start
