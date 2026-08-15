/* SPARC V9 trap and software-MMU assembly paths. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.section ".text", #alloc, #execinstr
	.register %g2, #ignore
	.register %g3, #ignore
	.balign 16
	.global sparcv9_fast_instruction_miss
	.type sparcv9_fast_instruction_miss, #function
sparcv9_fast_instruction_miss:
	mov 0x30, %g1
	ldxa [%g1] 0x50, %g1
	srlx %g1, 13, %g1
	sllx %g1, 13, %g1
	setx 0xfffff80000000000, %g2, %g3
	cmp %g1, %g3
	blu,pn %xcc, sparcv9_trap_entry_from_hw
	 nop
	sub %g1, %g3, %g4
	setx 0x20000000, %g2, %g5
	cmp %g4, %g5
	bgeu,pn %xcc, sparcv9_trap_entry_from_hw
	 nop
	srlx %g4, 22, %g4
	sllx %g4, 22, %g4
	setx 0xe000000000000035, %g2, %g5
	or %g4, %g5, %g5
	srlx %g4, 19, %g6
	and %g6, 0x1f8, %g6
	mov 64, %g3
1:
	ldxa [%g6] 0x55, %g2
	btst 0x40, %g2
	bz,pn %xcc, 2f
	 nop
	add %g6, 8, %g6
	and %g6, 0x1f8, %g6
	dec %g3
	brnz,pt %g3, 1b
	 nop
	ba,pn %xcc, sparcv9_trap_entry_from_hw
	 nop
2:
	stxa %g5, [%g6] 0x55
	membar #Sync
	retry
	 nop
	.size sparcv9_fast_instruction_miss, .-sparcv9_fast_instruction_miss

	.balign 16
	.global sparcv9_fast_data_miss
	.type sparcv9_fast_data_miss, #function
sparcv9_fast_data_miss:
	mov 0x30, %g1
	ldxa [%g1] 0x58, %g1
	srlx %g1, 13, %g1
	sllx %g1, 13, %g1
	setx 0xfffff80000000000, %g2, %g3
	cmp %g1, %g3
	blu,pn %xcc, sparcv9_data_miss_slow
	 nop
	sub %g1, %g3, %g4
	setx 0x20000000, %g2, %g5
	cmp %g4, %g5
	bgeu,pn %xcc, sparcv9_data_miss_slow
	 nop
	srlx %g4, 22, %g4
	sllx %g4, 22, %g4
	setx 0xe000000000000037, %g2, %g5
	or %g4, %g5, %g5
	srlx %g4, 19, %g6
	and %g6, 0x1f8, %g6
	mov 64, %g3
1:
	ldxa [%g6] 0x5d, %g2
	btst 0x40, %g2
	bz,pn %xcc, 2f
	 nop
	add %g6, 8, %g6
	and %g6, 0x1f8, %g6
	dec %g3
	brnz,pt %g3, 1b
	 nop
	ba,pn %xcc, sparcv9_data_miss_slow
	 nop
2:
	stxa %g5, [%g6] 0x5d
	membar #Sync
	retry
	 nop

sparcv9_data_miss_slow:
	ba,pn %xcc, sparcv9_trap_entry_from_hw
	 nop
	.size sparcv9_fast_data_miss, .-sparcv9_fast_data_miss

	.global sparcv9_trap_entry
	.type sparcv9_trap_entry, #function
sparcv9_trap_entry:
sparcv9_trap_entry_from_hw:
	rdpr %tt, %g1
	rdpr %tstate, %g2
	andcc %g2, 0x400, %g0
	be,pn %xcc, sparcv9_user_trap_entry
	 nop
	/* A miss raised by a user register-window spill is nested at TL=2. */
	cmp %g1, 0x68
	bne,pn %xcc, sparcv9_kernel_trap_entry
	 rdpr %tpc, %g3
	setx sparcv9_window_spill, %g5, %g6
	cmp %g3, %g6
	blu,pn %xcc, sparcv9_kernel_trap_entry
	 nop
	setx sparcv9_window_selftest, %g5, %g6
	cmp %g3, %g6
	bgeu,pn %xcc, sparcv9_kernel_trap_entry
	 nop

sparcv9_user_trap_entry:
	/* User traps switch to the current opaque task's kernel stack. */
	setx sparcv9_current_trap_sp, %g3, %g6
	ldx [%g6], %g6
	add %g6, 2047, %g3
	sub %g3, 64, %g3
	stx %sp, [%g3 + 0]
	mov %o5, %g5
	mov %o5, %g4
	set 0x16d, %g3
	cmp %g1, %g3
	bne,pn %xcc, 1f
	 nop
	mov %l0, %g4
1:
	setx sparcv9_current_trap_sp, %g3, %g6
	ldx [%g6], %g6
	add %g6, 2047, %g3
	sub %g3, 64, %g3
	stx %g5, [%g3 + 8]
	stx %o0, [%g3 + 16]
	stx %o1, [%g3 + 24]
	stx %o2, [%g3 + 32]
	stx %o3, [%g3 + 40]
	stx %o4, [%g3 + 48]
	stx %g4, [%g3 + 56]
	setx sparcv9_current_user_windows, %g3, %g5
	ldx [%g5], %g5
	stx %g7, [%g5 + 1128]
	rdpr %cwp, %g1
	stx %g1, [%g5 + 1024]
	rdpr %cansave, %g1
	stx %g1, [%g5 + 1032]
	rdpr %canrestore, %g1
	stx %g1, [%g5 + 1040]
	rdpr %otherwin, %g1
	stx %g1, [%g5 + 1048]
	rdpr %cleanwin, %g1
	stx %g1, [%g5 + 1056]
	rdpr %wstate, %g1
	stx %g1, [%g5 + 1064]
	clr %g2
3:
	wrpr %g2, 0, %cwp
	sllx %g2, 7, %g3
	add %g5, %g3, %g3
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
	blu,pt %xcc, 3b
	 nop
	/* Start a fresh kernel window chain on the task's trap stack. */
	clr %g2
4:
	wrpr %g2, 0, %cwp
	clr %l0
	clr %l1
	clr %l2
	clr %l3
	clr %l4
	clr %l5
	clr %l6
	clr %l7
	clr %i0
	clr %i1
	clr %i2
	clr %i3
	clr %i4
	clr %i5
	clr %i6
	clr %i7
	inc %g2
	cmp %g2, 8
	blu,pt %xcc, 4b
	 nop
	wrpr %g0, 0, %cwp
	wrpr %g0, 6, %cansave
	wrpr %g0, 0, %canrestore
	wrpr %g0, 0, %otherwin
	wrpr %g0, 7, %cleanwin
	wrpr %g0, 0, %wstate
	setx sparcv9_current_trap_sp, %g3, %g6
	ldx [%g6], %sp
	sub %sp, 256, %sp
	rdpr %tt, %o0
	rdpr %tpc, %o1
	rdpr %tnpc, %o2
	rdpr %tstate, %o3
	setx sparcv9_current_trap_sp, %g3, %g6
	ldx [%g6], %g6
	add %g6, 2047, %g3
	sub %g3, 64, %o4
	call sparcv9_user_trap_dispatch
	 nop
	mov %o0, %g4
	setx sparcv9_current_trap_sp, %g3, %g6
	ldx [%g6], %g6
	add %g6, 2047, %g3
	sub %g3, 64, %g3
	ldx [%g3 + 16], %g1
	setx sparcv9_current_user_windows, %g3, %g5
	ldx [%g5], %g5
	ldx [%g5 + 1024], %g2
	inc %g2
	and %g2, 7, %g2
	sllx %g2, 7, %g3
	add %g5, %g3, %g3
	stx %g1, [%g3 + 64]
	clr %g2
5:
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
	blu,pt %xcc, 5b
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
	ldx [%g5 + 1128], %g7
	cmp %g4, 1
	be,pn %xcc, 2f
	 nop
	retry
	 nop
2:
	done
	 nop

sparcv9_kernel_trap_entry:
	/* Generic kernel traps run on the interrupted biased stack. */
	save %sp, -256, %sp
	mov %g1, %o0
	rdpr %tpc, %o1
	rdpr %tnpc, %o2
	mov %g2, %o3
	call sparcv9_trap_dispatch
	 nop
	cmp %o0, 1
	be,pn %xcc, 1f
	 nop
	restore
	retry
	 nop
1:
	restore
	done
	 nop
	.size sparcv9_trap_entry, .-sparcv9_trap_entry
