/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit RISC-V user-space system call entry point and signal
 * restorer of the shared C library.
 */

#include <hal/arch.h>

	.text

/*
 * intptr_t __syscall6(number, a1, a2, a3, a4, a5, a6)
 *
 * The kernel takes the number in a7 and the arguments in a0 to a5, and
 * returns the result in a0.
 */
	.balign 4
	.global __syscall6
	.type __syscall6, %function
__syscall6:
	mv	a7, a0
	mv	a0, a1
	mv	a1, a2
	mv	a2, a3
	mv	a3, a4
	mv	a4, a5
	mv	a5, a6
	ecall
	ret
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
	/* The child reads its entry, argument and stack from s1 to s3. */
	addi	sp, sp, -32
	sd	s1, 0(sp)
	sd	s2, 8(sp)
	sd	s3, 16(sp)
	mv	s1, a0
	mv	s2, a1
	mv	s3, a2
	li	a7, 169
	ecall
	bnez	a0, 1f

	/* The child: its own stack before anything is written to one. */
	andi	sp, s3, -16
	li	s0, 0
	mv	a0, s2
	jalr	s1

	/* entry returned: the child ends with 127 (exit is system call 1). */
	li	a0, 127
	li	a7, 1
	ecall
	ebreak

	/* The parent, once the child execed or ended. */
1:	ld	s1, 0(sp)
	ld	s2, 8(sp)
	ld	s3, 16(sp)
	addi	sp, sp, 32
	ret
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
	lwu	a0, HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(sp)
	ld	a1, HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(sp)
	li	a7, 80
	ecall
	ebreak
	.size __signal_restorer, .-__signal_restorer

	.section .note.GNU-stack,"",%progbits
