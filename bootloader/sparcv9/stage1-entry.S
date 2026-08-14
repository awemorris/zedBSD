/*
 * OpenBoot disk bootstrap entry.  OpenFirmware passes the client interface
 * pointer in %o4 and loads this image at virtual address 0x4000.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

	.section ".text.entry", #alloc, #execinstr
	.align 4
	.global _start
	.type _start, #function
_start:
	save	%sp, -176, %sp
	setx	0x000000000000c000, %g1, %sp
	sub	%sp, 2047, %sp
	call	sparcv9_stage1_main
	 mov	%i4, %o0
1:
	ba	1b
	 nop
	.size _start, .-_start

