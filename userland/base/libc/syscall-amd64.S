/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the amd64 user-space system call entry point.
 */

#include <hal/arch.h>

.code64
.text
.globl __syscall6
.type __syscall6, @function
__syscall6:
	pushq %rbx
	pushq %rbp
	movq %rdi, %rax
	movq %rsi, %rbx
	movq %rdx, %r10
	movq %rcx, %r11
	movq %r10, %rcx
	movq %r11, %rdx
	movq %r8, %rsi
	movq %r9, %rdi
	movq 24(%rsp), %rbp
	int $0xc2
	popq %rbp
	popq %rbx
	ret
.size __syscall6, .-__syscall6

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movq HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%rsp), %rbx
	movq HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%rsp), %rcx
	movq $80, %rax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
