/*
 * Boots kernel-side entry, called by the HAL
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The HAL's cmain() calls kernel_entry() on the 4KB startup stack.
 * This switches to the kernel's own stack and enters the C bridge
 * (kern/kmain.c) with the boot handoff pointer the loader parked at
 * ADDR_BOOT_INFO (physical 0x2000; the direct map mirrors it at
 * SYS_START + 0x2000).  BSS was cleared by locore before any of this.
 *
 * This belongs to the HAL-linked BOOT.SYS.  It is not part of the
 * gateway-era image, whose entry stays in platform/pc98/stage2-entry.S,
 * because that path enters boots_main directly without a HAL beneath it.
 */

	.code32
	.section .text.entry, "ax"
	.globl kernel_entry
	.extern boots_kernel_main
kernel_entry:
	cld
	movl $__kernel_stack_top, %esp
	xorl %ebp, %ebp
	movl 0x80002000, %ebx	/* physical handoff address */
	pushl %ebx
	call boots_kernel_main
1:
	cli
	hlt
	jmp 1b

	.section .bss
	.balign 16
__kernel_stack:
	.space 32768
	.globl __kernel_stack_top
__kernel_stack_top:

	.section .note.GNU-stack, "", @progbits
