/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh second stage's entry.
 *
 * The boot block reads the second stage into memory the ROM's heap gave
 * it, wherever that is, and enters its first byte with the drive (d0),
 * the driver (d1), the partition's offset on the drive (d2) and the boot
 * block (a0).  The stage is linked at 0: the entry adds its own address
 * to every 32-bit address the tool that finished it listed, makes the
 * caches see the changed code, clears the BSS and enters C on the ROM's
 * stack.
 */

#define _ASM_SRC_ 1
#include "bootloader/mac68k/boot-layout.h"

/*
 * _start()
 */
	.section .text.start,"ax"
	.globl _start
	.type _start,@function
_start:
	bra.w stage2_entry
	.org MAC68K_STAGE2_MAGIC_OFFSET
	.long MAC68K_STAGE2_MAGIC
	.long 0
	.long 0
	.long 0
	.org MAC68K_STAGE2_HEADER_SIZE

stage2_entry:
	move.l %d0,%d3
	move.l %d1,%d4
	move.l %d2,%d5
	movea.l %a0,%a3

	/* Adds the stage's address to every listed address. */
	lea _start(%pc),%a1
	move.l %a1,%d2
	move.l MAC68K_STAGE2_RELOC_OFFSET_OFFSET(%a1),%d0
	lea 0(%a1,%d0.l),%a2
	move.l MAC68K_STAGE2_RELOC_COUNT_OFFSET(%a1),%d1
1:	subq.l #1,%d1
	bmi.s 2f
	move.l (%a2)+,%d0
	add.l %d2,0(%a1,%d0.l)
	bra.s 1b

	/* The changed code is in the data cache: pushes it and refills the instruction cache. */
2:	cpusha %bc
	nop
	bra.w 3f

	/* Clears the BSS, which may cover the list just used. */
3:	movea.l #__bss_start,%a2
	movea.l #__bss_end,%a1
4:	cmpa.l %a1,%a2
	bcc.s 5f
	clr.b (%a2)+
	bra.s 4b

	/* Enters C with what the boot block passed. */
5:	move.l %a3,-(%sp)
	move.l %d5,-(%sp)
	move.l %d4,-(%sp)
	move.l %d3,-(%sp)
	jsr mac68k_stage2_main
6:	bra.s 6b
	.size _start,.-_start
