/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC runtime-linker entry point and its system call veneer.
 *
 * The kernel enters _rtld_start with the stack as the program will see
 * it.  rtld_main relocates the loader, loads the program and its
 * libraries and returns the program's entry, which is entered with the
 * same stack.
 */

	.text
	.balign 16
	.global _rtld_start
	.type _rtld_start, @function
_rtld_start:
	/* Loads the program, keeping the stack it is to start on. */
	mr	%r31, %r1
	mr	%r3, %r1

	/* No frame and no return address: a stack walk stops here. */
	clrrwi	%r1, %r1, 4
	li	%r0, 0
	stwu	%r0, -16(%r1)
	mtlr	%r0
	bl	rtld_main

	/* Enters the program's entry with the kernel's stack. */
	mtctr	%r3
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
