/* MC68030 MMU register operations. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.even

	.globl m68k030_load_srp
	.type m68k030_load_srp,@function
m68k030_load_srp:
	move.l 4(%sp),%a0
	pmove (%a0),%srp
	rts
	.size m68k030_load_srp,.-m68k030_load_srp

	.globl m68k030_load_crp
	.type m68k030_load_crp,@function
m68k030_load_crp:
	move.l 4(%sp),%a0
	pmove (%a0),%crp
	rts
	.size m68k030_load_crp,.-m68k030_load_crp

	.globl m68k030_load_tc
	.type m68k030_load_tc,@function
m68k030_load_tc:
	move.l 4(%sp),%a0
	pmove (%a0),%tc
	rts
	.size m68k030_load_tc,.-m68k030_load_tc

	.globl m68k030_flush_atc
	.type m68k030_flush_atc,@function
m68k030_flush_atc:
	/* FC mask 0/mask 0 invalidates all non-global ATC entries. */
	pflush #0,#0
	rts
	.size m68k030_flush_atc,.-m68k030_flush_atc

	.globl m68k030_read_mmusr
	.type m68k030_read_mmusr,@function
m68k030_read_mmusr:
	/* GNU as names the 68030 MMUSR's compatible 16-bit view PSR. */
	clr.l %d0
	subq.l #2,%sp
	pmove %psr,(%sp)
	move.w (%sp)+,%d0
	rts
	.size m68k030_read_mmusr,.-m68k030_read_mmusr

	/* PTEST overwrites PSR/MMUSR for this exact logical address.  Reading
	 * PSR alone after a bus-error frame is unsafe because it may describe an
	 * earlier translation.  Function codes 1 and 2 are user data/program. */
	.globl m68k030_test_user_read
	.type m68k030_test_user_read,@function
m68k030_test_user_read:
	move.l 4(%sp),%a0
	ptestr #1,(%a0),#7
	bra.s .Lread_psr
	.size m68k030_test_user_read,.-m68k030_test_user_read

	.globl m68k030_test_user_write
	.type m68k030_test_user_write,@function
m68k030_test_user_write:
	move.l 4(%sp),%a0
	ptestw #1,(%a0),#7
	bra.s .Lread_psr
	.size m68k030_test_user_write,.-m68k030_test_user_write

	.globl m68k030_test_user_exec
	.type m68k030_test_user_exec,@function
m68k030_test_user_exec:
	move.l 4(%sp),%a0
	ptestr #2,(%a0),#7
.Lread_psr:
	clr.l %d0
	subq.l #2,%sp
	pmove %psr,(%sp)
	move.w (%sp)+,%d0
	rts
	.size m68k030_test_user_exec,.-m68k030_test_user_exec
