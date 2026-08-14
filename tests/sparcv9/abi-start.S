/*
 * SPARC V9 compiler ABI probe entry.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

	.section ".text"
	.align 4
	.global _start
	.type _start, #function
_start:
	save	%sp, -176, %sp
	mov	1, %o0
	mov	2, %o1
	mov	3, %o2
	mov	4, %o3
	mov	5, %o4
	call	sparcv9_abi_probe
	 mov	6, %o5
1:
	ba	1b
	 nop
	.size _start, .-_start
