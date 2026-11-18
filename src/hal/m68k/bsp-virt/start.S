/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first instructions on the virt board.
 *
 * The loader enters at the image's linked address in supervisor mode,
 * with translation off and interrupts masked, and leaves its boot
 * information right after the image (after the BSS too).  The entry
 * clears the BSS without a stack, takes its own stack and enters C with
 * the information's address.
 */

	.section .text.boot,"ax"
	.globl _start
	.type _start,@function
_start:
	move.w #0x2700,%sr

	/* C requires a zeroed BSS, which holds the stack. */
	lea __bss_start,%a0
	lea __bss_end,%a1
1:	cmpa.l %a1,%a0
	bcc.s 2f
	clr.l (%a0)+
	bra.s 1b
2:
	lea virt_boot_stack_top,%sp
	pea __kernel_end
	jsr virt_cmain
3:	stop #0x2700
	bra.s 3b
	.size _start,.-_start

	.section .bss.boot,"aw",@nobits
	.balign 16
virt_boot_stack:
	.space 16384
virt_boot_stack_top:
