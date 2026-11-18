/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC start file of a static program, with the system call
 * veneer and the signal restorer the static C library calls (the ELFv2
 * ABI).
 *
 * The kernel enters _start with the stack pointer at the argument count,
 * followed by the argument vector, its terminating null and the
 * environment, each a doubleword.  _start finds the TOC from its own
 * address, so it does not depend on r2 or r12 at entry.  The program's
 * calls keep the stack on the 16-byte boundary below a 32-byte minimum
 * frame whose back chain is zero, so a stack walk stops here; the frame's
 * TOC slot is where a call through the PLT saves r2.
 */

	.abiversion 2
	.text
	.balign 16
	.global _start
	.type _start, @function
_start:
	/* The TOC, from this code's own address. */
	bcl	20, 31, 1f
1:	mflr	%r2
	addis	%r2, %r2, (.TOC.-1b)@ha
	addi	%r2, %r2, (.TOC.-1b)@l

	/* The argument count, the argument vector and the environment, in registers calls keep. */
	mr	%r9, %r1
	ld	%r29, 0(%r9)
	addi	%r30, %r9, 8
	sldi	%r31, %r29, 3
	add	%r31, %r30, %r31
	addi	%r31, %r31, 8

	/* No frame and no return address: a stack walk stops here. */
	clrrdi	%r1, %r1, 4
	li	%r0, 0
	stdu	%r0, -32(%r1)
	mtlr	%r0

	/* Sets the C library up. */
	mr	%r3, %r29
	mr	%r4, %r30
	mr	%r5, %r31
	bl	__libc_init
	nop

	/* Runs the program with the same three arguments. */
	mr	%r3, %r29
	mr	%r4, %r30
	mr	%r5, %r31
	bl	main
	nop

	/*
	 * Leaves with main's status; exit runs the atexit handlers and
	 * flushes the streams, and does not return.
	 */
	bl	exit
	nop
2:
	trap
	b	2b
	.size _start, .-_start

#include "../syscall-ppc64.S"
