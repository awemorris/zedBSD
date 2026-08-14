/* Microsoft x64 UEFI call frame -> System V amd64 kernel transition. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
	.text
	.globl zbl_transition_start
	.globl zbl_transition_end
zbl_transition_start:
	cli
	movq %rdx, %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	movq %rcx, %cr3
	movq %r8, %rdi
	jmp *%r9
zbl_transition_end:
