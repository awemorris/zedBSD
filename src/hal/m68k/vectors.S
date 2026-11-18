/* High VBR table and common MC68030 exception entry. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "frame-offsets.h"

	.section .vectors,"a"
	.balign 1024
	.globl m68k_vector_table
m68k_vector_table:
	.rept 256
	.long m68k_exception_entry
	.endr

	.text
	.even
	.globl m68k_exception_entry
	.type m68k_exception_entry,@function
m68k_exception_entry:
	/* The complete, potentially 92-byte hardware frame remains below this
	 * fixed software prefix and is handed unchanged back to RTE. */
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	move.l %usp,%a0
	move.l %a0,-(%sp)
	move.l %sp,-(%sp)
	jsr m68k_exception_dispatch
	addq.l #4,%sp
	.globl m68k_exception_restore
m68k_exception_restore:
	move.l (%sp)+,%a0
	move.l %a0,%usp
	movem.l (%sp)+,%d0-%d7/%a0-%a6
	rte
	.size m68k_exception_entry,.-m68k_exception_entry

	.globl m68k_set_vbr
	.type m68k_set_vbr,@function
m68k_set_vbr:
	move.l 4(%sp),%d0
	movec %d0,%vbr
	rts
	.size m68k_set_vbr,.-m68k_set_vbr
