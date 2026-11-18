/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The common ground of the x86 processor decoders: the registers one
 * CPUID instruction returns, the bit-field helpers every decoder uses, and
 * the conventions all of them keep.
 *
 * The decoders turn the raw values a processor reports (CPUID registers,
 * model-specific registers, the hardware feedback table in memory) into
 * plain structures, and turn requests back into register values.  They
 * never execute an instruction, touch memory they were not given, or keep
 * state between calls: the driver runs CPUID, RDMSR and WRMSR itself and
 * hands the values over.  That keeps every decoder testable on the host.
 *
 * Conventions shared by every cpux86 header:
 *
 * - Errors are errno values from <uapi/errno.h>.  EINVAL: a NULL pointer, an
 *   argument outside the range the header states, or an input the
 *   specification calls malformed (a reserved bit that must be zero is set,
 *   a field outside its encoding).  ENOTSUP: the input is well formed but
 *   says the feature is absent (a leaf the processor does not implement, a
 *   model the allow list does not cover).  ERANGE: a conversion whose result
 *   does not fit the output type.  EACCES: a register the processor has
 *   locked against the change asked for.  Each function lists the ones it
 *   returns.
 * - On any error the output structure or value is left exactly as the
 *   caller passed it, unless the function says otherwise.
 * - Integer arithmetic only: no floating point and no library calls, since
 *   the kernel is built without floating-point registers.
 * - Every input is untrusted: no input value makes a decoder read or write
 *   outside the objects it was given, loop without bound, or execute
 *   undefined behaviour.  Shifts are by less than the width of the shifted
 *   type, products are formed in a type wide enough for both factors or
 *   checked first, and no signed arithmetic is used on register values.
 * - The language is ANSI C (C89); the headers compile on their own and
 *   twice in one unit.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/errno.h>

/*
 * The four registers one execution of CPUID returns, as the processor
 * returned them for one leaf and one sub-leaf.
 *
 * The driver fills one of these per leaf it reads, on the CPU the answer
 * is about; a decoder only reads it.
 */
struct cpux86_cpuid_regs {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};

/*
 * Extracts a field of a 32-bit register.
 *
 * Input: value, the register; low, the field's lowest bit (0..31); width,
 * the field's width in bits (1..32), with low + width <= 32.
 * Output: the field, right-aligned.  Any other low or width (including
 * width 0) returns 0; no shift reaches 32.
 * Errors: none.
 */
uint32_t
cpux86_field32(
	uint32_t value,
	uint32_t low,
	uint32_t width);

/*
 * Extracts a field of a 64-bit register.
 *
 * Input: value, the register; low, the field's lowest bit (0..63); width,
 * 1..64, with low + width <= 64.
 * Output: the field, right-aligned.  Any other low or width returns 0; no
 * shift reaches 64.
 * Errors: none.
 */
uint64_t
cpux86_field64(
	uint64_t value,
	uint32_t low,
	uint32_t width);

/*
 * Replaces a field of a 64-bit register.
 *
 * Input: value, the register; low and width, as for cpux86_field64();
 * field, the new contents, of which only the low width bits are used.
 * Output: value with bits low..low+width-1 replaced by the low width bits
 * of field and every other bit unchanged.  Any low or width that
 * cpux86_field64() would refuse returns value unchanged.
 * Errors: none.
 */
uint64_t
cpux86_insert64(
	uint64_t value,
	uint32_t low,
	uint32_t width,
	uint64_t field);

/*
 * Reports the number of bits needed to tell count things apart.
 *
 * This is the smallest s with 2^s >= count, the width the processor
 * reserves in an APIC ID for count instances when it rounds up to a power
 * of two.
 * Input: count, any value.
 * Output: 0 for count 0 and 1, s for 2 <= count <= 2^31, and 32 for any
 * larger count.
 * Errors: none.
 */
uint32_t
cpux86_ceil_log2(
	uint32_t count);

#endif
