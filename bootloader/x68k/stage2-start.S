/* X68000 stage 2 entry and one-way kernel transfer. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#define _ASM_SRC_ 1
#include "bootloader/x68k/boot-layout.h"

	.text
	.globl _start
	.type _start,@function
_start:
	movea.l #X68K_STAGE2_STACK,%sp
	move.l %a0,-(%sp)
	move.l %d4,-(%sp)
	jsr x68k_stage2_main
	addq.l #8,%sp
	moveq #-2,%d0
	trap #15
1:	bra 1b
	.size _start,.-_start

	.globl x68k_stage2_enter_kernel
	.type x68k_stage2_enter_kernel,@function
x68k_stage2_enter_kernel:
	movea.l 4(%sp),%a1
	movea.l 8(%sp),%a0
	move.l 12(%sp),%d4
	jmp (%a1)
	.size x68k_stage2_enter_kernel,.-x68k_stage2_enter_kernel
