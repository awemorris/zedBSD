/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC runtime-linker entry point and its system call
 * veneer (the ELFv2 ABI).
 *
 * The kernel enters _rtld_start with the stack as the program will see
 * it.  _rtld_start finds the loader's TOC from its own address;
 * rtld_main relocates the loader, loads the program and its libraries
 * and returns the program's entry, which is entered with the same stack
 * and with its address in r12, as a global entry point expects.
 */

	.abiversion 2
	.text
	.balign 16
	.global _rtld_start
	.type _rtld_start, @function
_rtld_start:
	/* The TOC, from this code's own address. */
	bcl	20, 31, 1f
1:	mflr	%r2
	addis	%r2, %r2, (.TOC.-1b)@ha
	addi	%r2, %r2, (.TOC.-1b)@l

	/* Loads the program, keeping the stack it is to start on. */
	mr	%r31, %r1
	mr	%r3, %r1

	/* No frame and no return address: a stack walk stops here. */
	clrrdi	%r1, %r1, 4
	li	%r0, 0
	stdu	%r0, -32(%r1)
	mtlr	%r0
	bl	rtld_main
	nop

	/* Enters the program's entry with the kernel's stack. */
	mtctr	%r3
	mr	%r12, %r3
	mr	%r1, %r31
	li	%r0, 0
	mtlr	%r0
	bctr
	.size _rtld_start, .-_rtld_start

/*
 * intptr_t rtld_syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in r0 and the arguments in r3 to r8, and
 * returns the result in r3.  The calling convention brings the number and
 * the six arguments in r3 to r9.
 */
	.balign 4
	.global rtld_syscall6
	.type rtld_syscall6, @function
rtld_syscall6:
	mr	%r0, %r3
	mr	%r3, %r4
	mr	%r4, %r5
	mr	%r5, %r6
	mr	%r6, %r7
	mr	%r7, %r8
	mr	%r8, %r9
	sc
	blr
	.size rtld_syscall6, .-rtld_syscall6

	.section .note.GNU-stack,"",%progbits
