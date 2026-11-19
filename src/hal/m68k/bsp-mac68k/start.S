/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first instructions on the Macintosh.
 *
 * The loader (the emulator's, or the boot loader on the machine) enters
 * at the image's linked address in supervisor mode, with translation and
 * the caches off, interrupts masked and the vector base at 0, and leaves
 * the boot information right after the image (after the BSS too).  The
 * stack pointer is not set.  The entry clears the BSS without a stack,
 * takes its own stack and enters C with the information's address.
 *
 * Procedure referenced: QEMU v10.0.0 hw/m68k/q800.c (the boot information
 * placed at the end of the loaded image, two-byte aligned).
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
	lea mac68k_boot_stack_top,%sp
	pea __kernel_end
	jsr mac68k_cmain
3:	stop #0x2700
	bra.s 3b
	.size _start,.-_start

	.section .bss.boot,"aw",@nobits
	.balign 16
mac68k_boot_stack:
	.space 16384
mac68k_boot_stack_top:
