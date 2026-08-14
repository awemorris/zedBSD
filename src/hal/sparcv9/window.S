/* Register-window spill/fill handlers. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.section ".text", #alloc, #execinstr
	.balign 16
	.global sparcv9_window_spill
sparcv9_window_spill:
	stx %l0, [%sp + 2047 + 0]
	stx %l1, [%sp + 2047 + 8]
	stx %l2, [%sp + 2047 + 16]
	stx %l3, [%sp + 2047 + 24]
	stx %l4, [%sp + 2047 + 32]
	stx %l5, [%sp + 2047 + 40]
	stx %l6, [%sp + 2047 + 48]
	stx %l7, [%sp + 2047 + 56]
	stx %i0, [%sp + 2047 + 64]
	stx %i1, [%sp + 2047 + 72]
	stx %i2, [%sp + 2047 + 80]
	stx %i3, [%sp + 2047 + 88]
	stx %i4, [%sp + 2047 + 96]
	stx %i5, [%sp + 2047 + 104]
	stx %i6, [%sp + 2047 + 112]
	stx %i7, [%sp + 2047 + 120]
	saved
	retry
	 nop

	.global sparcv9_window_fill
sparcv9_window_fill:
	ldx [%sp + 2047 + 0], %l0
	ldx [%sp + 2047 + 8], %l1
	ldx [%sp + 2047 + 16], %l2
	ldx [%sp + 2047 + 24], %l3
	ldx [%sp + 2047 + 32], %l4
	ldx [%sp + 2047 + 40], %l5
	ldx [%sp + 2047 + 48], %l6
	ldx [%sp + 2047 + 56], %l7
	ldx [%sp + 2047 + 64], %i0
	ldx [%sp + 2047 + 72], %i1
	ldx [%sp + 2047 + 80], %i2
	ldx [%sp + 2047 + 88], %i3
	ldx [%sp + 2047 + 96], %i4
	ldx [%sp + 2047 + 104], %i5
	ldx [%sp + 2047 + 112], %i6
	ldx [%sp + 2047 + 120], %i7
	restored
	retry
	 nop

	.global sparcv9_window_selftest
	.type sparcv9_window_selftest, #function
sparcv9_window_selftest:
	save %sp, -176, %sp
	.rept 16
	save %sp, -176, %sp
	.endr
	.rept 16
	restore
	.endr
	ret
	 restore
	.size sparcv9_window_selftest, .-sparcv9_window_selftest
