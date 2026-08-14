/* Opaque SPARC V9 task context switch. */
	.section ".text", #alloc, #execinstr
	.register %g2, #ignore
	.register %g3, #ignore
	.register %g6, #ignore
	.register %g7, #ignore
	.balign 16
	.global sparcv9_task_dispatch
	.type sparcv9_task_dispatch, #function
sparcv9_task_dispatch:
	/* Pointers live in globals because changing CWP changes the out registers. */
	stx %g1, [%o0 + 1080]
	stx %g2, [%o0 + 1088]
	stx %g3, [%o0 + 1096]
	stx %g4, [%o0 + 1104]
	stx %g5, [%o0 + 1112]
	stx %g6, [%o0 + 1120]
	stx %g7, [%o0 + 1128]
	mov %o0, %g4
	mov %o1, %g5
	rdpr %cwp, %g1
	stx %g1, [%g4 + 1024]
	rdpr %cansave, %g1
	stx %g1, [%g4 + 1032]
	rdpr %canrestore, %g1
	stx %g1, [%g4 + 1040]
	rdpr %otherwin, %g1
	stx %g1, [%g4 + 1048]
	rdpr %cleanwin, %g1
	stx %g1, [%g4 + 1056]
	rdpr %wstate, %g1
	stx %g1, [%g4 + 1064]
	clr %g2
1:
	wrpr %g2, 0, %cwp
	sllx %g2, 7, %g3
	add %g4, %g3, %g3
	stx %l0, [%g3 + 0]
	stx %l1, [%g3 + 8]
	stx %l2, [%g3 + 16]
	stx %l3, [%g3 + 24]
	stx %l4, [%g3 + 32]
	stx %l5, [%g3 + 40]
	stx %l6, [%g3 + 48]
	stx %l7, [%g3 + 56]
	stx %i0, [%g3 + 64]
	stx %i1, [%g3 + 72]
	stx %i2, [%g3 + 80]
	stx %i3, [%g3 + 88]
	stx %i4, [%g3 + 96]
	stx %i5, [%g3 + 104]
	stx %i6, [%g3 + 112]
	stx %i7, [%g3 + 120]
	inc %g2
	cmp %g2, 8
	blu,pt %xcc, 1b
	 nop
	clr %g2
2:
	wrpr %g2, 0, %cwp
	sllx %g2, 7, %g3
	add %g5, %g3, %g3
	ldx [%g3 + 0], %l0
	ldx [%g3 + 8], %l1
	ldx [%g3 + 16], %l2
	ldx [%g3 + 24], %l3
	ldx [%g3 + 32], %l4
	ldx [%g3 + 40], %l5
	ldx [%g3 + 48], %l6
	ldx [%g3 + 56], %l7
	ldx [%g3 + 64], %i0
	ldx [%g3 + 72], %i1
	ldx [%g3 + 80], %i2
	ldx [%g3 + 88], %i3
	ldx [%g3 + 96], %i4
	ldx [%g3 + 104], %i5
	ldx [%g3 + 112], %i6
	ldx [%g3 + 120], %i7
	inc %g2
	cmp %g2, 8
	blu,pt %xcc, 2b
	 nop
	ldx [%g5 + 1032], %g1
	wrpr %g1, 0, %cansave
	ldx [%g5 + 1040], %g1
	wrpr %g1, 0, %canrestore
	ldx [%g5 + 1048], %g1
	wrpr %g1, 0, %otherwin
	ldx [%g5 + 1056], %g1
	wrpr %g1, 0, %cleanwin
	ldx [%g5 + 1064], %g1
	wrpr %g1, 0, %wstate
	ldx [%g5 + 1024], %g1
	wrpr %g1, 0, %cwp
	ldx [%g5 + 1080], %g1
	ldx [%g5 + 1088], %g2
	ldx [%g5 + 1096], %g3
	ldx [%g5 + 1104], %g4
	ldx [%g5 + 1120], %g6
	ldx [%g5 + 1128], %g7
	ldx [%g5 + 1112], %g5
	retl
	 nop
	.size sparcv9_task_dispatch, .-sparcv9_task_dispatch

	.global sparcv9_kernel_task_entry
sparcv9_kernel_task_entry:
	mov %l1, %o0
	call %l0
	 nop
	call sparcv9_task_returned
	 nop

	.global sparcv9_user_task_entry
sparcv9_user_task_entry:
	/* Fault in the entry and initial stack while still on the task's kernel stack. */
	mov %l0, %o0
	call sparcv9_user_task_prepare
	 mov %l1, %o1
	flushw
	/* l0=entry, l1=unbiased user stack.  retry drops PRIV at TL=1. */
	sub %l1, 2047, %sp
	wrpr %g0, 1, %tl
	wrpr %l0, 0, %tpc
	add %l0, 4, %l2
	wrpr %l2, 0, %tnpc
	rdpr %cwp, %l2
	mov 2, %l3
	sllx %l3, 8, %l3
	or %l2, %l3, %l2
	wrpr %l2, 0, %tstate
	retry
	 nop
