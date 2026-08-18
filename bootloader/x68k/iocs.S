/* Minimal IOCS wrappers used only before the kernel takeover boundary. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.globl x68k_iocs_read_ext
	.type x68k_iocs_read_ext,@function
x68k_iocs_read_ext:
	movem.l %d2-%d5,-(%sp)
	move.l 20(%sp),%d2
	move.l 24(%sp),%d3
	move.l 28(%sp),%d4
	move.l 32(%sp),%d5
	movea.l 36(%sp),%a1
	moveq #-11,%d0
	moveq #38,%d1
	trap #15
	movem.l (%sp)+,%d2-%d5
	rts
	.size x68k_iocs_read_ext,.-x68k_iocs_read_ext

	.globl x68k_iocs_print
	.type x68k_iocs_print,@function
x68k_iocs_print:
	movea.l 4(%sp),%a1
	moveq #0x21,%d0
	trap #15
	rts
	.size x68k_iocs_print,.-x68k_iocs_print

	.globl x68k_iocs_fatal
	.type x68k_iocs_fatal,@function
x68k_iocs_fatal:
	movea.l 4(%sp),%a1
	moveq #0x21,%d0
	trap #15
	moveq #-2,%d0
	trap #15
1:	bra 1b
	.size x68k_iocs_fatal,.-x68k_iocs_fatal
