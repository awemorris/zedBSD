/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit ARM start file of a dynamic, position-independent program.
 *
 * The runtime linker has relocated the program and its libraries and
 * enters _start in the ARM instruction set with the stack as the kernel
 * left it: the argument count, the argument vector, its terminating null
 * and the environment.  The C library's set-up and the program's main
 * follow; exit runs the handlers the program registered.
 */

	.syntax unified
	.arm
	.text
	.balign 16
	.global _start
	.type _start, %function
_start:
	/* No frame and no return address: a stack walk stops here. */
	mov	fp, #0
	mov	lr, #0

	/* The argument count, the argument vector and the environment. */
	mov	r4, sp
	ldr	r5, [r4]
	add	r6, r4, #4
	add	r7, r6, r5, lsl #2
	add	r7, r7, #4

	/* Calls keep the stack on the 8-byte boundary the calling convention wants. */
	bic	sp, r4, #7

	/* Sets the C library up. */
	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	bl	__libc_init

	/* Runs the program with the same three arguments. */
	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	bl	main

	/* Leaves with main's status; exit does not return. */
	bl	exit
1:
	udf	#0
	b	1b
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
