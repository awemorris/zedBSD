/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

/*
  * Real-mode AP trampoline copied to physical 0x8000.
 */

#include "defs.h"

#define TRAMPOLINE_BASE AMD64_AP_TRAMPOLINE

.section .rodata.aptramp,"a"
.balign 16
.global amd64_ap_trampoline_start
amd64_ap_trampoline_start:
.code16
	cli
	cld
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	lgdt TRAMPOLINE_BASE + (ap_gdt_descriptor - amd64_ap_trampoline_start)
	movl TRAMPOLINE_BASE + (amd64_ap_trampoline_cr3 - amd64_ap_trampoline_start), %eax
	movl %eax, %cr3
	movl %cr4, %eax
	orl $0x20, %eax
	movl %eax, %cr4
	movl $0xc0000080, %ecx
	rdmsr
	/* The BSP page tables already contain NX leaf entries. */
	orl $0x900, %eax
	wrmsr
	movl %cr0, %eax
	orl $0x80000001, %eax
	movl %eax, %cr0
	ljmpl *TRAMPOLINE_BASE + (ap_far_pointer - amd64_ap_trampoline_start)

.balign 8
ap_gdt:
	.quad 0
	.quad 0x00af9a000000ffff
	.quad 0x00cf92000000ffff
ap_gdt_end:
ap_gdt_descriptor:
	.word ap_gdt_end - ap_gdt - 1
	.long TRAMPOLINE_BASE + (ap_gdt - amd64_ap_trampoline_start)
ap_far_pointer:
	.long TRAMPOLINE_BASE + (ap_long_mode - amd64_ap_trampoline_start)
	.word 0x08

.balign 8
.global amd64_ap_trampoline_cr3
amd64_ap_trampoline_cr3:
	.long 0

.code64
ap_long_mode:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movabs $TRAMPOLINE_BASE + (amd64_ap_trampoline_stack - amd64_ap_trampoline_start), %rax
	movq (%rax), %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	movabs $TRAMPOLINE_BASE + (amd64_ap_trampoline_cpu - amd64_ap_trampoline_start), %rax
	movq (%rax), %rdi
	movabs $TRAMPOLINE_BASE + (amd64_ap_trampoline_entry - amd64_ap_trampoline_start), %rax
	call *(%rax)
1:
	cli
	hlt
	jmp 1b

.balign 8
.global amd64_ap_trampoline_stack
amd64_ap_trampoline_stack:
	.quad 0
.global amd64_ap_trampoline_cpu
amd64_ap_trampoline_cpu:
	.quad 0
.global amd64_ap_trampoline_entry
amd64_ap_trampoline_entry:
	.quad 0
.global amd64_ap_trampoline_end
amd64_ap_trampoline_end:
