/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Integer-only IEEE 754 binary32 and binary64 arithmetic.
 *
 * These are the operations the compiler ABI wrappers and libc call.  Values
 * cross this boundary as their object representations rather than as C
 * floating-point types, so the implementation cannot accidentally recurse
 * through the compiler helpers that call it.
 */

#ifndef KERN_ZED_SOFTFLOAT_H
#define KERN_ZED_SOFTFLOAT_H

#include <stdint.h>

uint32_t zsf32_add(uint32_t left, uint32_t right);
uint32_t zsf32_sub(uint32_t left, uint32_t right);
uint32_t zsf32_mul(uint32_t left, uint32_t right);
uint32_t zsf32_div(uint32_t left, uint32_t right);
int zsf32_compare(uint32_t left, uint32_t right, int *unordered);

uint64_t zsf64_add(uint64_t left, uint64_t right);
uint64_t zsf64_sub(uint64_t left, uint64_t right);
uint64_t zsf64_mul(uint64_t left, uint64_t right);
uint64_t zsf64_div(uint64_t left, uint64_t right);
int zsf64_compare(uint64_t left, uint64_t right, int *unordered);

uint64_t zsf32_to_64(uint32_t value);
uint32_t zsf64_to_32(uint64_t value);
uint32_t zsf_i64_to_32(int64_t value);
uint32_t zsf_u64_to_32(uint64_t value);
uint64_t zsf_i64_to_64(int64_t value);
uint64_t zsf_u64_to_64(uint64_t value);
int64_t zsf32_to_i64(uint32_t value, unsigned int width, int unsigned_result);
int64_t zsf64_to_i64(uint64_t value, unsigned int width, int unsigned_result);
uint32_t zsf32_round_pack(unsigned int sign, int exponent, uint64_t significand);
uint64_t zsf64_round_pack(unsigned int sign, int exponent, uint64_t significand);

#endif
