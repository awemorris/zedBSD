/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC user-space system call entry point and signal
 * restorer of the shared C library.
 */

#include <hal/arch.h>

	.text

/*
 * intptr_t __syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in r0 and the arguments in r3 to r8, and
 * returns the result in r3.  The calling convention brings the number and
 * the six arguments in r3 to r9.  sc keeps every other register.
 */
	.balign 4
	.global __syscall6
	.type __syscall6, @function
__syscall6:
	mr	%r0, %r3
	mr	%r3, %r4
	mr	%r4, %r5
	mr	%r5, %r6
	mr	%r6, %r7
	mr	%r7, %r8
	mr	%r8, %r9
	sc
	blr
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
	.type __vfork_spawn, @function
__vfork_spawn:
	/* A frame keeping r29 to r31, which carry the entry, argument and stack. */
	stwu	%r1, -32(%r1)
	mflr	%r0
	stw	%r0, 36(%r1)
	stw	%r29, 20(%r1)
	stw	%r30, 24(%r1)
	stw	%r31, 28(%r1)
	mr	%r29, %r3
	mr	%r30, %r4
	mr	%r31, %r5
	li	%r0, 169
	sc
	cmpwi	%r3, 0
	bne	1f

	/* The child: its own stack, ending the back chain, before anything is written to one. */
	clrrwi	%r1, %r31, 4
	li	%r0, 0
	stwu	%r0, -16(%r1)
	mtctr	%r29
	mr	%r3, %r30
	bctrl

	/* entry returned: the child ends with 127 (exit is system call 1). */
	li	%r3, 127
	li	%r0, 1
	sc
	trap

	/* The parent, once the child execed or ended. */
1:	lwz	%r29, 20(%r1)
	lwz	%r30, 24(%r1)
	lwz	%r31, 28(%r1)
	lwz	%r0, 36(%r1)
	mtlr	%r0
	addi	%r1, %r1, 32
	blr
	.size __vfork_spawn, .-__vfork_spawn

/*
 * A signal handler returns here.  The stack pointer is back at the frame
 * the kernel built, whose token and context the return request names.
 * System call 80 is the signal return; it does not come back.
 */
	.balign 4
	.global __signal_restorer
	.type __signal_restorer, @function
__signal_restorer:
	/* Names the frame by its token and context, and asks for the return. */
	lwz	%r3, HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%r1)
	lwz	%r4, HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%r1)
	li	%r0, 80
	sc
	trap
	.size __signal_restorer, .-__signal_restorer

	.section .note.GNU-stack,"",%progbits
