/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit RISC-V start file of a dynamic, position-independent program.
 *
 * The runtime linker has relocated the program and its libraries and
 * enters _start with the stack as the kernel left it: the argument count,
 * the argument vector, its terminating null and the environment.  The C
 * library's set-up and the program's main follow; exit runs the handlers
 * the program registered.  Nothing is addressed through the global
 * pointer, since every object is built without linker relaxation.
 */

	.text
	.balign 16
	.global _start
	.type _start, %function
_start:
	.cfi_startproc
	/*
	 * Nothing called this, so a debugger walking the stack stops here:
	 * the register it would return through holds no return address.
	 */
	.cfi_undefined ra
	mv	s0, zero
	mv	ra, zero

	/* The argument count, the argument vector and the environment. */
	mv	s1, sp
	ld	s2, 0(s1)
	addi	s3, s1, 8
	slli	t0, s2, 3
	add	s4, s3, t0
	addi	s4, s4, 8

	/* Sets the C library up. */
	mv	a0, s2
	mv	a1, s3
	mv	a2, s4
	call	__libc_init

	/* Runs the program with the same three arguments. */
	mv	a0, s2
	mv	a1, s3
	mv	a2, s4
	call	main

	/* Leaves with main's status; exit does not return. */
	call	exit
1:
	ebreak
	j	1b
	.cfi_endproc
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
