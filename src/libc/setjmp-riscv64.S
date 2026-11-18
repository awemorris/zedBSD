/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Nonlocal jumps for 64-bit RISC-V.
 *
 * clang does not implement __builtin_setjmp for RISC-V, so the saved state
 * is written out by hand.  What has to survive is what the calling
 * convention makes the callee's responsibility: s0 to s11, the return
 * address, the stack pointer and, with the double-precision floating-point
 * ABI, fs0 to fs11.
 *
 * The buffer is an array of 8-byte slots; the C side sizes it.  Slots 0 to
 * 11 hold s0 to s11, 12 the return address, 13 the stack pointer and 14 to
 * 25 fs0 to fs11.
 */

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
	sd	s0, 0(a0)
	sd	s1, 8(a0)
	sd	s2, 16(a0)
	sd	s3, 24(a0)
	sd	s4, 32(a0)
	sd	s5, 40(a0)
	sd	s6, 48(a0)
	sd	s7, 56(a0)
	sd	s8, 64(a0)
	sd	s9, 72(a0)
	sd	s10, 80(a0)
	sd	s11, 88(a0)
	sd	ra, 96(a0)
	sd	sp, 104(a0)
#if defined(__riscv_flen) && __riscv_flen >= 64
	fsd	fs0, 112(a0)
	fsd	fs1, 120(a0)
	fsd	fs2, 128(a0)
	fsd	fs3, 136(a0)
	fsd	fs4, 144(a0)
	fsd	fs5, 152(a0)
	fsd	fs6, 160(a0)
	fsd	fs7, 168(a0)
	fsd	fs8, 176(a0)
	fsd	fs9, 184(a0)
	fsd	fs10, 192(a0)
	fsd	fs11, 200(a0)
#endif
	li	a0, 0
	ret
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
	ld	s0, 0(a0)
	ld	s1, 8(a0)
	ld	s2, 16(a0)
	ld	s3, 24(a0)
	ld	s4, 32(a0)
	ld	s5, 40(a0)
	ld	s6, 48(a0)
	ld	s7, 56(a0)
	ld	s8, 64(a0)
	ld	s9, 72(a0)
	ld	s10, 80(a0)
	ld	s11, 88(a0)
	ld	ra, 96(a0)
	ld	sp, 104(a0)
#if defined(__riscv_flen) && __riscv_flen >= 64
	fld	fs0, 112(a0)
	fld	fs1, 120(a0)
	fld	fs2, 128(a0)
	fld	fs3, 136(a0)
	fld	fs4, 144(a0)
	fld	fs5, 152(a0)
	fld	fs6, 160(a0)
	fld	fs7, 168(a0)
	fld	fs8, 176(a0)
	fld	fs9, 184(a0)
	fld	fs10, 192(a0)
	fld	fs11, 200(a0)
#endif
	mv	a0, a1
	ret
	.size __zed_longjmp, .-__zed_longjmp

	.section .note.GNU-stack,"",%progbits
