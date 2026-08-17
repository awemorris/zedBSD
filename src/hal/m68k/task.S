/* MC68030 callee-saved dispatch and initial task return paths. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.even
	.globl m68k_task_dispatch
	.type m68k_task_dispatch,@function
m68k_task_dispatch:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
	movem.l %d2-%d7/%a2-%a6,-(%sp)
	move.l %sp,(%a0)
	move.l %a1,%sp
	movem.l (%sp)+,%d2-%d7/%a2-%a6
	rts
	.size m68k_task_dispatch,.-m68k_task_dispatch

	.globl m68k_kernel_task_start
	.type m68k_kernel_task_start,@function
m68k_kernel_task_start:
	move.l (%sp)+,%a0             /* function */
	move.l (%sp)+,-(%sp)          /* one C argument */
	jsr (%a0)
	addq.l #4,%sp
1:	stop #0x2700
	bra.s 1b
	.size m68k_kernel_task_start,.-m68k_kernel_task_start

	.globl m68k_user_task_start
	.type m68k_user_task_start,@function
m68k_user_task_start:
	move.l (%sp)+,%d0             /* user PC */
	move.l (%sp)+,%a0             /* user SP */
	move.l %a0,%usp
	clr.w -(%sp)                  /* format 0 / vector offset 0 */
	move.l %d0,-(%sp)
	clr.w -(%sp)                  /* user, IPL 0, trace clear */
	rte
	.size m68k_user_task_start,.-m68k_user_task_start

	.globl m68k_cpu_idle
	.type m68k_cpu_idle,@function
m68k_cpu_idle:
	stop #0x2000
	ori.w #0x0700,%sr
	rts
	.size m68k_cpu_idle,.-m68k_cpu_idle
