/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
	.text
	.globl __zedbsd_tlsdesc_resolver
	.hidden __zedbsd_tlsdesc_resolver
	.type __zedbsd_tlsdesc_resolver,%function
__zedbsd_tlsdesc_resolver:
	/* AArch64 TLSDESC may clobber x0 only.  x0 already holds the descriptor. */
	sub sp, sp, #160
	stp x1, x2, [sp, #0]
	stp x3, x4, [sp, #16]
	stp x5, x6, [sp, #32]
	stp x7, x8, [sp, #48]
	stp x9, x10, [sp, #64]
	stp x11, x12, [sp, #80]
	stp x13, x14, [sp, #96]
	stp x15, x16, [sp, #112]
	stp x17, x18, [sp, #128]
	stp x29, x30, [sp, #144]
	bl __zedbsd_tlsdesc_resolve
	ldp x1, x2, [sp, #0]
	ldp x3, x4, [sp, #16]
	ldp x5, x6, [sp, #32]
	ldp x7, x8, [sp, #48]
	ldp x9, x10, [sp, #64]
	ldp x11, x12, [sp, #80]
	ldp x13, x14, [sp, #96]
	ldp x15, x16, [sp, #112]
	ldp x17, x18, [sp, #128]
	ldp x29, x30, [sp, #144]
	add sp, sp, #160
	ret
	.size __zedbsd_tlsdesc_resolver,.-__zedbsd_tlsdesc_resolver
	.section .note.GNU-stack,"",%progbits
