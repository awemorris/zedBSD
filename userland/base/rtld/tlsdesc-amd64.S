/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the amd64 runtime-linker TLSDESC resolver.
 */

	.text
	.globl d_tlsdesc_resolver
	.hidden d_tlsdesc_resolver
	.type d_tlsdesc_resolver,@function
d_tlsdesc_resolver:
	/* The TLSDESC ABI preserves every register except the result in %rax. */
	pushq %rdi
	pushq %rsi
	pushq %rdx
	pushq %rcx
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	subq $8, %rsp
	movq %rax, %rdi
	call d_tlsdesc_resolve
	addq $8, %rsp
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rcx
	popq %rdx
	popq %rsi
	popq %rdi
	ret
	.size d_tlsdesc_resolver,.-d_tlsdesc_resolver
	.section .note.GNU-stack,"",@progbits
