/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the sparcv9 user-space system call entry point.
 */

#include <hal/arch.h>

	.section ".text"
.align 4
.global __syscall6
.type __syscall6, #function
__syscall6:
	/*
	 * The call's own window: the number and the first five arguments
	 * come in %i0-%i5, the sixth on the caller's stack.  The trap takes
	 * the number in %o5, the arguments in %o0-%o4 and the sixth in %l0,
	 * and returns the result in %o0.
	 */
	save	%sp, -176, %sp
	mov	%i1, %o0
	mov	%i2, %o1
	mov	%i3, %o2
	mov	%i4, %o3
	mov	%i5, %o4
	ldx	[%fp + 2223], %l0
	mov	%i0, %o5
	ta	0x6d
	ret
	 restore %o0, 0, %o0
.size __syscall6, .-__syscall6

.global __signal_restorer
.type __signal_restorer, #function
__signal_restorer:
	add %sp, HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET, %o1
	ldx [%o1], %o1
	ldx [%sp + HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET], %o2
	mov 80, %o0
	call __syscall6
	 nop
	ta 0x70
.size __signal_restorer, .-__signal_restorer
