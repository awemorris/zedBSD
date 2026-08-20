/* Position-independent INIT/SIPI trampoline copied to physical 0x6000. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "defs.h"

.section .rodata.ap_trampoline,"a"
.balign 16
.code16
.global i386_ap_trampoline_start
i386_ap_trampoline_start:
	cli
	xorw %ax,%ax
	movw %ax,%ds
	lgdtw %cs:(i386_ap_trampoline_gdtr-i386_ap_trampoline_start)
	movl %cr0,%eax
	orl $1,%eax
	movl %eax,%cr0
	ljmp $0x08,$(.Lprotected-i386_ap_trampoline_start)
.code32
.Lprotected:
	movw $0x10,%ax
	movw %ax,%ds
	movw %ax,%es
	movw %ax,%ss
	movl (i386_ap_trampoline_cr3-i386_ap_trampoline_start),%eax
	movl %eax,%cr3
	movl %cr0,%eax
	orl $0x80000000,%eax
	movl %eax,%cr0
	ljmpl *(i386_ap_trampoline_target-i386_ap_trampoline_start)

.balign 8
.global i386_ap_trampoline_cr3
i386_ap_trampoline_cr3: .long 0
.global i386_ap_trampoline_stack
i386_ap_trampoline_stack: .long 0
.global i386_ap_trampoline_cpu
i386_ap_trampoline_cpu: .long 0
.global i386_ap_trampoline_target
i386_ap_trampoline_target: .long 0
	.word 0x18
i386_ap_trampoline_gdtr:
	.word 5*8-1
	.long 0x6000+(i386_ap_trampoline_gdt-i386_ap_trampoline_start)
i386_ap_trampoline_gdt:
	.quad 0
	/* Base 0x6000, 32-bit, 4GB code/data. */
	.byte 0xff,0xff,0x00,0x60,0x00,0x9a,0xcf,0x00
	.byte 0xff,0xff,0x00,0x60,0x00,0x92,0xcf,0x00
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
.global i386_ap_trampoline_end
i386_ap_trampoline_end:

.text
.code32
.global i386_ap_high_entry
i386_ap_high_entry:
	movw $0x20,%ax
	movw %ax,%ds
	movw %ax,%es
	movw %ax,%ss
	movl i386_ap_trampoline_stack-i386_ap_trampoline_start+0x80006000,%esp
	xorl %ebp,%ebp
	pushl i386_ap_trampoline_cpu-i386_ap_trampoline_start+0x80006000
	call i386_smp_ap_entry
.Lhalt: cli;hlt;jmp .Lhalt
