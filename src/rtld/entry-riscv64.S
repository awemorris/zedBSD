/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit RISC-V runtime-linker entry point and its system call veneer.
 *
 * The kernel enters _rtld_start with the stack as the program will see it.
 * rtld_main relocates the loader, loads the program and its libraries and
 * returns the program's entry, which is entered with the same stack.
 */

	.text
	.balign 4
	.global _rtld_start
	.type _rtld_start, %function
_rtld_start:
	/* No frame and no return address: a stack walk stops here. */
	mv	s0, zero
	mv	ra, zero

	/* Loads the program, keeping the stack it is to start on. */
	mv	s1, sp
	mv	a0, s1
	call	rtld_main

	/* Enters the program's entry with the kernel's stack. */
	mv	sp, s1
	jr	a0
	.size _rtld_start, .-_rtld_start

/*
 * intptr_t rtld_syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in a7 and the arguments in a0 to a5, and
 * returns the result in a0.
 */
	.balign 4
	.global rtld_syscall6
	.type rtld_syscall6, %function
rtld_syscall6:
	mv	a7, a0
	mv	a0, a1
	mv	a1, a2
	mv	a2, a3
	mv	a3, a4
	mv	a4, a5
	mv	a5, a6
	ecall
	ret
	.size rtld_syscall6, .-rtld_syscall6

	.section .note.GNU-stack,"",%progbits
