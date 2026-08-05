/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

	.code32
	.section .text.entry, "ax"
	.globl _start32
	.extern boots_main
	.extern __low_bss_start
	.extern __low_bss_end
	.extern __high_bss_start
	.extern __high_bss_end
_start32:
	cld
	movl $0x0008f000, %esp
	xorl %ebp, %ebp
	/* Stage 1 loads only PT_LOAD file bytes; both BSS regions are ours. */
	xorl %eax, %eax
	movl $__low_bss_start, %edi
	movl $__low_bss_end, %ecx
	subl %edi, %ecx
	rep stosb
	movl $__high_bss_start, %edi
	movl $__high_bss_end, %ecx
	subl %edi, %ecx
	rep stosb
	pushl %ebx		/* Stage 1 handoff pointer */
	call boots_main
1:
	cli
	hlt
	jmp 1b

	.section .note.GNU-stack, "", @progbits
