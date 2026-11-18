/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit ARM runtime-linker entry point and its system call veneer.
 *
 * The kernel enters _rtld_start in the ARM instruction set with the stack
 * as the program will see it.  rtld_main relocates the loader, loads the
 * program and its libraries and returns the program's entry, which is
 * entered with the same stack.
 */

	.syntax unified
	.arm
	.text
	.balign 16
	.global _rtld_start
	.type _rtld_start, %function
_rtld_start:
	/* No frame and no return address: a stack walk stops here. */
	mov	fp, #0
	mov	lr, #0

	/* Loads the program, keeping the stack it is to start on. */
	mov	r4, sp
	bic	sp, r4, #7
	mov	r0, r4
	bl	rtld_main

	/* Enters the program's entry with the kernel's stack. */
	mov	sp, r4
	bx	r0
	.size _rtld_start, .-_rtld_start

/*
 * intptr_t rtld_syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in r7 and the arguments in r0 to r5, and
 * returns the result in r0.  The calling convention brings the first
 * three arguments in r1 to r3 and the last three on the stack, and has
 * the callee keep r4, r5 and r7.
 */
	.balign 4
	.global rtld_syscall6
	.type rtld_syscall6, %function
rtld_syscall6:
	push	{r4, r5, r7, lr}
	mov	r7, r0
	mov	r0, r1
	mov	r1, r2
	mov	r2, r3
	ldr	r3, [sp, #16]
	ldr	r4, [sp, #20]
	ldr	r5, [sp, #24]
	svc	#0
	pop	{r4, r5, r7, pc}
	.size rtld_syscall6, .-rtld_syscall6

	.section .note.GNU-stack,"",%progbits
