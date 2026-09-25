/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the amd64 runtime-linker entry point.
 */

.code64
.text
.globl _rtld_start
.type _rtld_start, @function
_rtld_start:
	xorq %rbp, %rbp
	movq %rsp, %r12
	movq %r12, %rdi
	call rtld_main
	movq %r12, %rsp
	jmp *%rax
.size _rtld_start, .-_rtld_start

.globl rtld_syscall6
.type rtld_syscall6, @function
rtld_syscall6:
	pushq %rbx
	pushq %rbp
	movq %rdi, %rax
	movq %rsi, %rbx
	movq %rdx, %r10
	movq %rcx, %rdx
	movq %r8, %rsi
	movq %r9, %rdi
	movq 24(%rsp), %rbp
	/*
	 * SYSCALL takes the second argument in r10, since it keeps the return
	 * address in rcx; rcx and r11 do not survive the call.
	 */
	syscall
	popq %rbp
	popq %rbx
	ret
.size rtld_syscall6, .-rtld_syscall6
