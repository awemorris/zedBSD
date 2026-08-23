/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ZED_SOFTFLOAT_H
#define ZEDBSD_ZED_SOFTFLOAT_H

#include <stdint.h>

/* Integer-only IEEE 754 operations used by the compiler ABI wrappers and
 * libc.  Values cross this boundary as their object representations so the
 * implementation cannot accidentally recurse through compiler helpers. */
uint32_t zsf32_add(uint32_t, uint32_t);
uint32_t zsf32_sub(uint32_t, uint32_t);
uint32_t zsf32_mul(uint32_t, uint32_t);
uint32_t zsf32_div(uint32_t, uint32_t);
int zsf32_compare(uint32_t, uint32_t, int *);

uint64_t zsf64_add(uint64_t, uint64_t);
uint64_t zsf64_sub(uint64_t, uint64_t);
uint64_t zsf64_mul(uint64_t, uint64_t);
uint64_t zsf64_div(uint64_t, uint64_t);
int zsf64_compare(uint64_t, uint64_t, int *);

uint64_t zsf32_to_64(uint32_t);
uint32_t zsf64_to_32(uint64_t);
uint32_t zsf_i64_to_32(int64_t);
uint32_t zsf_u64_to_32(uint64_t);
uint64_t zsf_i64_to_64(int64_t);
uint64_t zsf_u64_to_64(uint64_t);
int64_t zsf32_to_i64(uint32_t, unsigned int, int);
int64_t zsf64_to_i64(uint64_t, unsigned int, int);
uint32_t zsf32_round_pack(unsigned int, int, uint64_t);
uint64_t zsf64_round_pack(unsigned int, int, uint64_t);

#endif
