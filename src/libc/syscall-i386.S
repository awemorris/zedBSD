/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the i386 user-space system call entry point.
 */

#include <hal/arch.h>

.code32
.text
.globl __syscall6
.type __syscall6, @function
__syscall6:
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %ebx
	movl 20(%esp), %eax
	movl 24(%esp), %ebx
	movl 28(%esp), %ecx
	movl 32(%esp), %edx
	movl 36(%esp), %esi
	movl 40(%esp), %edi
	movl 44(%esp), %ebp
	int $0xc2
	popl %ebx
	popl %esi
	popl %edi
	popl %ebp
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
.globl __vfork_spawn
.hidden __vfork_spawn
.type __vfork_spawn, @function
__vfork_spawn:
	/* The child reads its entry, argument and stack from esi, edi and ebp. */
	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi
	movl 20(%esp), %esi
	movl 24(%esp), %edi
	movl 28(%esp), %ebp
	movl $169, %eax
	int $0xc2
	testl %eax, %eax
	jnz 1f

	/* The child: its own stack before anything is written to one. */
	movl %ebp, %esp
	andl $-16, %esp
	xorl %ebp, %ebp
	subl $12, %esp
	pushl %edi
	call *%esi

	/* entry returned: the child ends with 127 (exit is system call 1). */
	movl $1, %eax
	movl $127, %ebx
	int $0xc2
	ud2

	/* The parent, once the child execed or ended. */
1:	popl %edi
	popl %esi
	popl %ebx
	popl %ebp
	ret
.size __vfork_spawn, .-__vfork_spawn

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movl HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%esp), %ebx
	movl HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%esp), %ecx
	movl $80, %eax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
