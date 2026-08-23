/* zedBSD m68k user entry, syscall veneer, and signal restorer. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/arch.h>

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
	jsr __libc_init
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
	.globl __syscall6
	.type __syscall6,@function
__syscall6:
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
	.size __syscall6,.-__syscall6

	.globl __signal_restorer
	.type __signal_restorer,@function
__signal_restorer:
	move.l HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%sp),%d1
	move.l HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%sp),%d2
	moveq #80,%d0                   /* ZEDBSD_SYS_sigreturn */
	trap #0
	illegal
	.size __signal_restorer,.-__signal_restorer
