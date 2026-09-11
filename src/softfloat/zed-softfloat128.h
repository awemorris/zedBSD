/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Integer-only IEEE 754 binary128 arithmetic for SPARC V9.
 *
 * SPARC V9 names quad-precision operations in its ABI that no zedBSD target
 * implements in hardware.  Values cross this boundary as their object
 * representations, so the implementation cannot recurse through the compiler
 * helpers that call it.
 */

#ifndef KERN_ZED_SOFTFLOAT128_H
#define KERN_ZED_SOFTFLOAT128_H

#include <stdint.h>

/*
 * One binary128 object representation, as its two halves.
 *
 * The high half carries the sign, the exponent field, and the top 48 bits of
 * the fraction; the low half carries the remaining 64.
 */
struct zsf128 {
	uint64_t high;
	uint64_t low;
};

struct zsf128 zsf128_add(struct zsf128 left, struct zsf128 right);
struct zsf128 zsf128_sub(struct zsf128 left, struct zsf128 right);
struct zsf128 zsf128_mul(struct zsf128 left, struct zsf128 right);
struct zsf128 zsf128_div(struct zsf128 left, struct zsf128 right);
int zsf128_compare(struct zsf128 left, struct zsf128 right, int *unordered);

struct zsf128 zsf32_to_128(uint32_t value);
struct zsf128 zsf64_to_128(uint64_t value);
uint32_t zsf128_to_32(struct zsf128 value);
uint64_t zsf128_to_64(struct zsf128 value);
struct zsf128 zsf_i64_to_128(int64_t value);
struct zsf128 zsf_u64_to_128(uint64_t value);

#endif
