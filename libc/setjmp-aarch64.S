/* -*- mode: asm; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Nonlocal jumps for AArch64.
 *
 * Every other target reaches these through the compiler's own
 * __builtin_setjmp, but clang does not implement that builtin for
 * AArch64, so the saved state is written out by hand. What has to
 * survive is exactly what the procedure call standard makes the
 * callee's responsibility: x19 through x28, the frame pointer, the
 * link register, the stack pointer, and the low halves of v8 to v15.
 *
 * The buffer is an array of 8-byte slots. The C side sizes it.
 */

/*
 * The FP registers below are callee-saved state this code has to
 * preserve for its caller. The kernel compiles its C with
 * -mgeneral-regs-only so the compiler never emits FP of its own; this
 * states that the assembler may.
 */
	.arch armv8-a+fp+simd

	.text

/*
 * int __zed_setjmp(void *context)
 *
 * Records the caller's resumable state and returns 0.
 */
	.balign 16
	.global __zed_setjmp
	.type __zed_setjmp,%function
__zed_setjmp:
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x1, sp
	str x1, [x0, #96]
	stp d8, d9, [x0, #104]
	stp d10, d11, [x0, #120]
	stp d12, d13, [x0, #136]
	stp d14, d15, [x0, #152]
	mov w0, #0
	ret
	.size __zed_setjmp,.-__zed_setjmp

/*
 * void __zed_longjmp(void *context, int value)
 *
 * Restores the recorded state and returns value from the matching
 * __zed_setjmp. The caller has already forced a zero value to one.
 */
	.balign 16
	.global __zed_longjmp
	.type __zed_longjmp,%function
__zed_longjmp:
	ldp x19, x20, [x0, #0]
	ldp x21, x22, [x0, #16]
	ldp x23, x24, [x0, #32]
	ldp x25, x26, [x0, #48]
	ldp x27, x28, [x0, #64]
	ldp x29, x30, [x0, #80]
	ldr x2, [x0, #96]
	ldp d8, d9, [x0, #104]
	ldp d10, d11, [x0, #120]
	ldp d12, d13, [x0, #136]
	ldp d14, d15, [x0, #152]
	mov sp, x2
	mov w0, w1
	ret
	.size __zed_longjmp,.-__zed_longjmp

	.section .note.GNU-stack,"",%progbits
