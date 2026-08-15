/* MC68030 X68000 reboot trampoline. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#define DIRECT_BASE             0x80000000
#define SHORT_TABLE_DESCRIPTOR  0x00000002

	.section .text.boot,"ax"
	.globl x68k_machine_reset
	.type x68k_machine_reset,@function
x68k_machine_reset:
	move.w #0x2700,%sr

	/* Disable both MC68030 caches before changing translation tables. */
	moveq #0,%d0
	movec %d0,%cacr

	/* Restore a temporary identity map for physical 0..4 MiB.  The first
	 * direct-map leaf already describes that RAM and contains this routine. */
	lea m68k030_bootstrap_root+DIRECT_BASE,%a0
	move.l #m68k030_bootstrap_direct_leaf+SHORT_TABLE_DESCRIPTOR,(%a0)
	pflush #0,#0

	/* MMU-off execution must already be using an identity-mapped PC. */
	lea .Lidentity-DIRECT_BASE,%a0
	jmp (%a0)

.Lidentity:
	/* PMOVE requires a memory operand.  No stack access follows MMU disable. */
	subq.l #4,%sp
	clr.l (%sp)
	pmove (%sp),%tc

	/* The reset SSP/PC live at the start of IPL ROM.  Also restore vectors at
	 * physical zero for accelerator boards which do not provide reset overlay. */
	move.l 0x00ff0000,%d0
	move.l 0x00ff0004,%a0
	move.l %d0,0x00000000
	move.l %a0,0x00000004
	moveq #0,%d1
	movec %d1,%vbr
	move.l %d0,%sp
	jmp (%a0)
	.size x68k_machine_reset,.-x68k_machine_reset
