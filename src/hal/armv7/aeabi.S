/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The run-time helpers of the ARM EABI the compiler calls in the kernel
 * that cannot be written in C: the 64-bit division, which returns the
 * quotient in r0:r1 and the remainder in r2:r3, and the memory functions,
 * which take their arguments in another order than the C library's (or
 * fewer of them) and go to the kernel's own memcpy, memmove and memset.
 * The 32-bit division is aeabi-divide.c.  The 4- and 8-byte variants of a memory
 * function only promise an aligned destination, so they are the same code.
 *
 * Values referenced: ARM IHI 0043 (Run-time ABI for the ARM Architecture),
 * the names and register use of __aeabi_uldivmod, __aeabi_ldivmod,
 * __aeabi_memcpy, __aeabi_memmove, __aeabi_memset (destination, count,
 * value), __aeabi_memclr and __aeabi_ldiv0.
 */

#include "defs.h"

	.syntax unified
	.arm
	.text

	/*
	 * Divides r0:r1 by r2:r3 without sign: the quotient in r0:r1 and the
	 * remainder in r2:r3, through __udivmoddi4, which writes the remainder
	 * where its fifth word points.  A zero divisor goes to __aeabi_ldiv0.
	 */
	.balign 4
	.global __aeabi_uldivmod
	.type __aeabi_uldivmod, %function
__aeabi_uldivmod:
	orrs r12, r2, r3
	beq armv7_aeabi_ldiv0
	push {r4, lr}
	sub sp, sp, #16
	add r4, sp, #8
	str r4, [sp]
	bl __udivmoddi4
	ldr r2, [sp, #8]
	ldr r3, [sp, #12]
	add sp, sp, #16
	pop {r4, pc}
	.size __aeabi_uldivmod, . - __aeabi_uldivmod

	/*
	 * Divides r0:r1 by r2:r3 with sign, as __aeabi_uldivmod does, through
	 * __divmoddi4: the quotient rounds toward zero and the remainder has
	 * the dividend's sign.
	 */
	.balign 4
	.global __aeabi_ldivmod
	.type __aeabi_ldivmod, %function
__aeabi_ldivmod:
	orrs r12, r2, r3
	beq armv7_aeabi_ldiv0
	push {r4, lr}
	sub sp, sp, #16
	add r4, sp, #8
	str r4, [sp]
	bl __divmoddi4
	ldr r2, [sp, #8]
	ldr r3, [sp, #12]
	add sp, sp, #16
	pop {r4, pc}
	.size __aeabi_ldivmod, . - __aeabi_ldivmod

	/* A 64-bit division by zero stops the kernel. */
armv7_aeabi_ldiv0:
	b __aeabi_ldiv0

	/* Copies r2 bytes from r1 to r0, which do not overlap. */
	.balign 4
	.global __aeabi_memcpy
	.global __aeabi_memcpy4
	.global __aeabi_memcpy8
	.type __aeabi_memcpy, %function
	.type __aeabi_memcpy4, %function
	.type __aeabi_memcpy8, %function
__aeabi_memcpy:
__aeabi_memcpy4:
__aeabi_memcpy8:
	b memcpy
	.size __aeabi_memcpy, . - __aeabi_memcpy
	.size __aeabi_memcpy4, . - __aeabi_memcpy4
	.size __aeabi_memcpy8, . - __aeabi_memcpy8

	/* Copies r2 bytes from r1 to r0, which may overlap. */
	.balign 4
	.global __aeabi_memmove
	.global __aeabi_memmove4
	.global __aeabi_memmove8
	.type __aeabi_memmove, %function
	.type __aeabi_memmove4, %function
	.type __aeabi_memmove8, %function
__aeabi_memmove:
__aeabi_memmove4:
__aeabi_memmove8:
	b memmove
	.size __aeabi_memmove, . - __aeabi_memmove
	.size __aeabi_memmove4, . - __aeabi_memmove4
	.size __aeabi_memmove8, . - __aeabi_memmove8

	/* Fills r1 bytes at r0 with the byte r2: memset takes the value before the count. */
	.balign 4
	.global __aeabi_memset
	.global __aeabi_memset4
	.global __aeabi_memset8
	.type __aeabi_memset, %function
	.type __aeabi_memset4, %function
	.type __aeabi_memset8, %function
__aeabi_memset:
__aeabi_memset4:
__aeabi_memset8:
	mov r3, r1
	mov r1, r2
	mov r2, r3
	b memset
	.size __aeabi_memset, . - __aeabi_memset
	.size __aeabi_memset4, . - __aeabi_memset4
	.size __aeabi_memset8, . - __aeabi_memset8

	/* Clears r1 bytes at r0. */
	.balign 4
	.global __aeabi_memclr
	.global __aeabi_memclr4
	.global __aeabi_memclr8
	.type __aeabi_memclr, %function
	.type __aeabi_memclr4, %function
	.type __aeabi_memclr8, %function
__aeabi_memclr:
__aeabi_memclr4:
__aeabi_memclr8:
	mov r2, r1
	mov r1, #0
	b memset
	.size __aeabi_memclr, . - __aeabi_memclr
	.size __aeabi_memclr4, . - __aeabi_memclr4
	.size __aeabi_memclr8, . - __aeabi_memclr8

	/*
	 * Calls __aeabi_uldivmod with r0:r1 and r2:r3 and stores the four
	 * result registers where the fifth word points, in order: the
	 * self-test reads the remainder the C calling convention cannot.
	 */
	.balign 4
	.global armv7_aeabi_call_uldivmod
	.type armv7_aeabi_call_uldivmod, %function
armv7_aeabi_call_uldivmod:
	push {r4, lr}
	ldr r4, [sp, #8]
	bl __aeabi_uldivmod
	stmia r4, {r0-r3}
	pop {r4, pc}
	.size armv7_aeabi_call_uldivmod, . - armv7_aeabi_call_uldivmod

	/* The same for __aeabi_ldivmod. */
	.balign 4
	.global armv7_aeabi_call_ldivmod
	.type armv7_aeabi_call_ldivmod, %function
armv7_aeabi_call_ldivmod:
	push {r4, lr}
	ldr r4, [sp, #8]
	bl __aeabi_ldivmod
	stmia r4, {r0-r3}
	pop {r4, pc}
	.size armv7_aeabi_call_ldivmod, . - armv7_aeabi_call_ldivmod
