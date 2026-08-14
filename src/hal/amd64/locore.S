/* zedBSD amd64 bootstrap entry. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "defs.h"

.code64
.section .text.entry,"ax"
.global amd64_start
.type amd64_start, @function
amd64_start:
	cli
	cld
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
