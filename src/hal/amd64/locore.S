/* zedBSD amd64 bootstrap entry. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "defs.h"
#include "bootloader/include/amd64-handoff.h"

.code64
.section .text.entry,"ax"
.global amd64_start
.type amd64_start, @function
amd64_start:
	cli
	cld
	/* Stage 4: the first kernel instruction is executing under bootstrap CR3. */
	cmpl $ZBL6_HANDOFF_MAGIC, (%rdi)
	jne .Lentry_stage_done
	cmpw $ZBL6_HANDOFF_V4_VERSION, 4(%rdi)
	jne .Lentry_stage_done
	movq ZBL6_HANDOFF_V3_FRAMEBUFFER_BASE_OFFSET(%rdi), %r8
	andq $0x1fffff, %r8
	movabsq $ZBL6_FRAMEBUFFER_VIRTUAL_BASE, %rax
	addq %rax, %r8
	movl ZBL6_HANDOFF_V3_FRAMEBUFFER_WIDTH_OFFSET(%rdi), %eax
	subl $136, %eax
	shlq $2, %rax
	addq %rax, %r8
	movl ZBL6_HANDOFF_V3_FRAMEBUFFER_STRIDE_OFFSET(%rdi), %r9d
	leaq 8(%r8,%r9,8), %r8
	addq $416, %r8
	shlq $2, %r9
	movl $16, %ecx
.Lentry_stage_row:
	movl $24, %eax
	movq %r8, %r10
.Lentry_stage_pixel:
	movl $0x00ffffff, (%r10)
	addq $4, %r10
	decl %eax
	jnz .Lentry_stage_pixel
	addq %r9, %r8
	decl %ecx
	jnz .Lentry_stage_row
	sfence
.Lentry_stage_done:
	leaq __amd64_boot_stack_top(%rip), %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	call amd64_cmain
.Lhalt:
	cli
	hlt
	jmp .Lhalt
.size amd64_start, .-amd64_start

.text
.global amd64_load_gdt
.type amd64_load_gdt, @function
amd64_load_gdt:
	lgdt (%rdi)
	pushq $SEG_KERNEL_CODE
	leaq .Lgdt_loaded(%rip), %rax
	pushq %rax
	lretq
.Lgdt_loaded:
	movw $SEG_KERNEL_DATA, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movw $SEG_TSS, %ax
	ltr %ax
	ret
.size amd64_load_gdt, .-amd64_load_gdt

.section .bss
.balign 16
__amd64_boot_stack:
	.skip 65536
.global __amd64_boot_stack_top
__amd64_boot_stack_top:
