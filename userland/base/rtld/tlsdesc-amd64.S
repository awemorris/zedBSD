/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
	.text
	.globl __zedbsd_tlsdesc_resolver
	.hidden __zedbsd_tlsdesc_resolver
	.type __zedbsd_tlsdesc_resolver,@function
__zedbsd_tlsdesc_resolver:
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
	call __zedbsd_tlsdesc_resolve
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
	.size __zedbsd_tlsdesc_resolver,.-__zedbsd_tlsdesc_resolver
	.section .note.GNU-stack,"",@progbits
