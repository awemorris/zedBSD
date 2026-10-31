/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit ARM start file of a static program, with the system call
 * veneer and the signal restorer the static C library calls.
 *
 * The kernel enters _start in the ARM instruction set with the stack
 * pointer at the argument count, followed by the argument vector, its
 * terminating null and the environment.
 */

#include <hal/arch.h>

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

	/* Leaves with main's status; _exit does not return. */
	bl	_exit
1:
	udf	#0
	b	1b
	.size _start, .-_start

/*
 * intptr_t __syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in r7 and the arguments in r0 to r5, and
 * returns the result in r0.  The calling convention brings the first
 * three arguments in r1 to r3 and the last three on the stack, and has
 * the callee keep r4, r5 and r7.
 */
	.balign 4
	.global __syscall6
	.type __syscall6, %function
__syscall6:
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
	.size __syscall6, .-__syscall6

/*
 * long __vfork_spawn(void (*entry)(void *), void *argument, void *stack_top);
 *
 * Starts a child that shares this address space until it execs or ends
 * (the vfork system call, 169), and runs entry(argument) in the child on
 * the stack below stack_top, so that the child never writes this
 * thread's stack.  The caller resumes when the child has execed or
 * ended, with the child's pid, or a negative errno.  entry must not
 * return; if it does, the child exits with status 127.
 */
	.balign 4
	.global __vfork_spawn
	.hidden __vfork_spawn
	.type __vfork_spawn, %function
__vfork_spawn:
	/* The child reads its entry, argument and stack from r4 to r6; r7 is the call number. */
	push	{r4, r5, r6, r7, r8, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
	mov	r7, #169
	svc	#0
	cmp	r0, #0
	bne	1f

	/* The child: its own stack before anything is written to one. */
	bic	r6, r6, #15
	mov	sp, r6
	mov	fp, #0
	mov	r0, r5
	blx	r4

	/* entry returned: the child ends with 127 (exit is system call 1). */
	mov	r0, #127
	mov	r7, #1
	svc	#0
	udf	#0

	/* The parent, once the child execed or ended. */
1:	pop	{r4, r5, r6, r7, r8, pc}
	.size __vfork_spawn, .-__vfork_spawn

/*
 * A signal handler returns here.  The stack pointer is back at the frame
 * the kernel built, whose token and context the return request names.
 * System call 80 is the signal return; it does not come back.
 */
	.balign 4
	.global __signal_restorer
	.type __signal_restorer, %function
__signal_restorer:
	/* Names the frame by its token and context, and asks for the return. */
	ldr	r0, [sp, #HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET]
	ldr	r1, [sp, #HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET]
	mov	r7, #80
	svc	#0
	udf	#0
	.size __signal_restorer, .-__signal_restorer

	.section .note.GNU-stack,"",%progbits
