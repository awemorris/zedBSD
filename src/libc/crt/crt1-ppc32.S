/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC start file of a dynamic, position-independent program.
 *
 * The runtime linker has relocated the program and its libraries and
 * enters _start with the stack as the kernel left it: the argument count,
 * the argument vector, its terminating null and the environment.  The C
 * library's set-up and the program's main follow; exit runs the handlers
 * the program registered.  The calls go through the program's PLT, whose
 * stubs find their slots from r30, the address of the global offset
 * table.
 */

	.text
	.balign 16
	.global _start
	.type _start, @function
_start:
	/* The argument count, the argument vector and the environment, in registers calls keep. */
	mr	%r9, %r1
	lwz	%r27, 0(%r9)
	addi	%r28, %r9, 4
	slwi	%r29, %r27, 2
	add	%r29, %r28, %r29
	addi	%r29, %r29, 4

	/* No frame and no return address: a stack walk stops here. */
	clrrwi	%r1, %r1, 4
	li	%r0, 0
	stwu	%r0, -16(%r1)

	/* The global offset table's address, which the PLT stubs read their slots from. */
	bcl	20, 31, 1f
1:	mflr	%r30
	addis	%r30, %r30, _GLOBAL_OFFSET_TABLE_-1b@ha
	addi	%r30, %r30, _GLOBAL_OFFSET_TABLE_-1b@l
	li	%r0, 0
	mtlr	%r0

	/* Sets the C library up. */
	mr	%r3, %r27
	mr	%r4, %r28
	mr	%r5, %r29
	bl	__libc_init@plt

	/* Runs the program with the same three arguments. */
	mr	%r3, %r27
	mr	%r4, %r28
	mr	%r5, %r29
	bl	main@plt

	/* Leaves with main's status; exit does not return. */
	bl	exit@plt
2:
	trap
	b	2b
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
