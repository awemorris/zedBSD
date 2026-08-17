/* zedBSD m68k user entry, syscall veneer, and signal restorer. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.even
	.globl _start
	.type _start,@function
_start:
	move.l %sp,%a2
	move.l (%a2),%d2                 /* argc */
	lea 4(%a2),%a3                   /* argv */
	lea 4(%a3,%d2.l*4),%a4          /* envp, past argv NULL */
	move.l %a4,-(%sp)
	move.l %a3,-(%sp)
	move.l %d2,-(%sp)
	jsr zedbsd_user_libc_init
	lea 12(%sp),%sp
	move.l %a4,-(%sp)
	move.l %a3,-(%sp)
	move.l %d2,-(%sp)
	jsr main
	lea 12(%sp),%sp
	move.l %d0,-(%sp)
	jsr _exit
1:	bra.s 1b
	.size _start,.-_start

	/* Public C ABI takes seven stack arguments.  The kernel ABI is D0 =
	 * number, D1-D5/A0 = arguments.  D2-D5 are callee-saved by the GNU m68k
	 * ABI and therefore must survive the trap veneer. */
	.globl zedbsd_syscall6
	.type zedbsd_syscall6,@function
zedbsd_syscall6:
	movem.l %d2-%d5,-(%sp)
	move.l 20(%sp),%d0
	move.l 24(%sp),%d1
	move.l 28(%sp),%d2
	move.l 32(%sp),%d3
	move.l 36(%sp),%d4
	move.l 40(%sp),%d5
	move.l 44(%sp),%a0
	trap #0
	movem.l (%sp)+,%d2-%d5
	rts
	.size zedbsd_syscall6,.-zedbsd_syscall6

	.globl zedbsd_signal_restorer
	.type zedbsd_signal_restorer,@function
zedbsd_signal_restorer:
	move.l 4(%sp),%d1               /* opaque context token */
	moveq #80,%d0                   /* ZEDBSD_SYS_sigreturn */
	trap #0
	illegal
	.size zedbsd_signal_restorer,.-zedbsd_signal_restorer
