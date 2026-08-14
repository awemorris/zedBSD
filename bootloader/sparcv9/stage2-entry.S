/*
 * zedBSD SPARC V9 second-stage entry.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

	.section ".text.entry", #alloc, #execinstr
	.align 4
	.global _start
	.type _start, #function
_start:
	save	%sp, -176, %sp
	setx	stage2_stack_top, %g1, %sp
	sub	%sp, 2047, %sp
	setx	__bss_start, %g1, %l0
	setx	__bss_end, %g1, %l1
1:
	cmp	%l0, %l1
	bgeu	2f
	 nop
	stx	%g0, [%l0]
	ba	1b
	 add	%l0, 8, %l0
2:
	mov	%i1, %o1
	call	sparcv9_stage2_main
	 mov	%i0, %o0
3:
	ba	3b
	 nop
	.size _start, .-_start

	.global sparcv9_loader_enable_mmu
	.type sparcv9_loader_enable_mmu, #function
sparcv9_loader_enable_mmu:
	mov	0xf, %o0
	stxa	%o0, [%g0] 0x45
	membar	#Sync
	retl
	 nop
	.size sparcv9_loader_enable_mmu, .-sparcv9_loader_enable_mmu

	.section ".bss.stack", #alloc, #write
	.align 4
stage2_stack:
	.skip 32768
stage2_stack_top:
	/* Keep ABI register-window spill slots away from the following BSS. */
	.skip 4096
