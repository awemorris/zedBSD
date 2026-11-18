/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Nonlocal jumps for 32-bit ARM.
 *
 * The saved state is written out by hand, as on AArch64 and RISC-V, so
 * that it is exactly what the calling convention makes the callee's
 * responsibility: r4 to r11, the stack pointer, the link register and,
 * with the floating-point unit's calling convention, d8 to d15.
 *
 * The buffer is an array of 4-byte slots; the C side sizes it.  Slots 0
 * to 7 hold r4 to r11, 8 the stack pointer, 9 the link register and 10 to
 * 25 d8 to d15, two slots each.
 */

	.syntax unified
	.arm
	.fpu	neon-vfpv3
	.text

/*
 * int __zed_setjmp(void *context)
 *
 * Records the caller's resumable state and returns 0.
 */
	.balign 4
	.global __zed_setjmp
	.type __zed_setjmp, %function
__zed_setjmp:
	mov	ip, r0
	stmia	ip!, {r4-r11}
	str	sp, [ip], #4
	str	lr, [ip], #4
	vstmia	ip, {d8-d15}
	mov	r0, #0
	bx	lr
	.size __zed_setjmp, .-__zed_setjmp

/*
 * void __zed_longjmp(void *context, int value)
 *
 * Restores the recorded state and returns value from the matching
 * __zed_setjmp.  The caller has already forced a zero value to one.
 */
	.balign 4
	.global __zed_longjmp
	.type __zed_longjmp, %function
__zed_longjmp:
	mov	ip, r0
	ldmia	ip!, {r4-r11}
	ldr	sp, [ip], #4
	ldr	lr, [ip], #4
	vldmia	ip, {d8-d15}
	mov	r0, r1
	bx	lr
	.size __zed_longjmp, .-__zed_longjmp

	.section .note.GNU-stack,"",%progbits
