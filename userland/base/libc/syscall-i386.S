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

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movl HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%esp), %ebx
	movl HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%esp), %ecx
	movl $80, %eax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
