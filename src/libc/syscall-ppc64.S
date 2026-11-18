/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC user-space system call entry point and signal
 * restorer of the shared C library (the ELFv2 ABI).
 *
 * None of these functions reads the TOC, so each is entered at its one
 * entry point whatever r2 holds.
 */

#include <hal/arch.h>

	.abiversion 2
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
 * return; if it does, the child exits with status 127.  The entry is
 * called at its global entry point, with its address in r12.
 */
	.balign 4
	.global __vfork_spawn
	.hidden __vfork_spawn
	.type __vfork_spawn, @function
__vfork_spawn:
	/* A frame keeping r29 to r31, which carry the entry, argument and stack. */
	mflr	%r0
	std	%r0, 16(%r1)
	stdu	%r1, -64(%r1)
	std	%r29, 32(%r1)
	std	%r30, 40(%r1)
	std	%r31, 48(%r1)
	mr	%r29, %r3
	mr	%r30, %r4
	mr	%r31, %r5
	li	%r0, 169
	sc
	cmpdi	%r3, 0
	bne	1f

	/* The child: its own stack, ending the back chain, before anything is written to one. */
	clrrdi	%r1, %r31, 4
	li	%r0, 0
	stdu	%r0, -32(%r1)
	mtctr	%r29
	mr	%r12, %r29
	mr	%r3, %r30
	bctrl

	/* entry returned: the child ends with 127 (exit is system call 1). */
	li	%r3, 127
	li	%r0, 1
	sc
	trap

	/* The parent, once the child execed or ended. */
1:	ld	%r29, 32(%r1)
	ld	%r30, 40(%r1)
	ld	%r31, 48(%r1)
	addi	%r1, %r1, 64
	ld	%r0, 16(%r1)
	mtlr	%r0
	blr
	.size __vfork_spawn, .-__vfork_spawn

/*
 * A signal handler returns here.  The stack pointer is back where the
 * handler was entered, and the kernel's frame with the token and the
 * context the return request names is where the processor's layer says.
 * System call 80 is the signal return; it does not come back.
 */
	.balign 4
	.global __signal_restorer
	.type __signal_restorer, @function
__signal_restorer:
	/* Names the frame by its token and context, and asks for the return. */
	lwz	%r3, HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%r1)
	ld	%r4, HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%r1)
	li	%r0, 80
	sc
	trap
	.size __signal_restorer, .-__signal_restorer

	.section .note.GNU-stack,"",%progbits
