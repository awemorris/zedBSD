/*
 * zedBSD SPARC V9 user entry and syscall veneer.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <hal/arch.h>

	.section ".text"
	.align 4
	.global _start
	.type _start, #function
_start:
	clr	%fp
	add	%sp, 2047, %l0
	ldx	[%l0], %l1
	add	%l0, 8, %l2
	sllx	%l1, 3, %l3
	add	%l2, %l3, %l3
	add	%l3, 8, %l3
	/* Reserve the ABI caller frame before the first non-leaf call. */
	sub	%sp, 176, %sp
	mov	%l1, %o0
	mov	%l2, %o1
	call	__libc_init
	 mov	%l3, %o2
	mov	%l1, %o0
	mov	%l2, %o1
	call	main
	 mov	%l3, %o2
	call	_exit
	 nop
1:
	ba	1b
	 nop
	.size _start, .-_start

	.global __syscall6
	.type __syscall6, #function
__syscall6:
	mov	%o0, %g1
	mov	%o1, %o0
	mov	%o2, %o1
	mov	%o3, %o2
	mov	%o4, %o3
	mov	%o5, %o4
	ldx	[%sp + 2223], %o5
	stx	%l0, [%sp + 2231]
	mov	%o5, %l0
	mov	%g1, %o5
	ta	0x6d
	ldx	[%sp + 2231], %l0
	retl
	 nop
	.size __syscall6, .-__syscall6

	.global __signal_restorer
	.type __signal_restorer, #function
__signal_restorer:
	/* Token follows the 128-byte register-window spill area. */
	add	%sp, HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET, %o1
	ldx	[%o1], %o1
	ldx	[%sp + HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET], %o2
	mov	80, %o0
	call	__syscall6
	 nop
	ta	0x70
	.size __signal_restorer, .-__signal_restorer
