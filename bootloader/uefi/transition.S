/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transition from the Microsoft x64 UEFI call frame to the System V
 * amd64 kernel entry.
 */

#include "../include/amd64-handoff.h"

	.text
	.globl zbl_transition_start
	.globl zbl_transition_quiet
	.globl zbl_transition_end
zbl_transition_start:
	cli
	movq %rdx, %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	movq %rcx, %cr3
	/* Draws the third progress block, which shows the bootstrap CR3 maps the framebuffer. */
	movq ZBL6_HANDOFF_V3_FRAMEBUFFER_BASE_OFFSET(%r8), %r10
	andq $0x1fffff, %r10
	movabsq $ZBL6_FRAMEBUFFER_VIRTUAL_BASE, %rax
	addq %rax, %r10
	movl ZBL6_HANDOFF_V3_FRAMEBUFFER_WIDTH_OFFSET(%r8), %eax
	subl $136, %eax
	shlq $2, %rax
	addq %rax, %r10
	movl ZBL6_HANDOFF_V3_FRAMEBUFFER_STRIDE_OFFSET(%r8), %r11d
	leaq 8(%r10,%r11,8), %r10
	addq $288, %r10
	shlq $2, %r11
	movl $16, %ecx
.Ltransition_stage_row:
	movl $24, %eax
	movq %r10, %rsi
.Ltransition_stage_pixel:
	movl $0x00ffffff, (%rsi)
	addq $4, %rsi
	decl %eax
	jnz .Ltransition_stage_pixel
	addq %r11, %r10
	decl %ecx
	jnz .Ltransition_stage_row
	sfence
.Ltransition_enter:
	movq %r8, %rdi
	jmp *%r9
	/*
	 * A quiet boot (kmsg=quiet) enters here: the same switch without the
	 * third progress block, so the boot logo stays on the screen.
	 */
zbl_transition_quiet:
	cli
	movq %rdx, %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	movq %rcx, %cr3
	jmp .Ltransition_enter
zbl_transition_end:
